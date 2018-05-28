// Copyright (c) 2011-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bitcoingui.h"

#include "clientmodel.h"
#include "guiutil.h"
#include "guiconstants.h"
#include "init.h"
#include "networkstyle.h"
#include "optionsmodel.h"
#include "splashscreen.h"
#include "ui_interface.h"
#include "walletmodel.h"

#include <QApplication>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QTextCodec>
#include <QTimer>
#include <QThread>
#include <QTranslator>
#include <QSplashScreen>

#if defined(BITCOIN_NEED_QT_PLUGINS)
#include <QtPlugin>
#if QT_VERSION < 0x050000
Q_IMPORT_PLUGIN(qcncodecs)
Q_IMPORT_PLUGIN(qjpcodecs)
Q_IMPORT_PLUGIN(qtwcodecs)
Q_IMPORT_PLUGIN(qkrcodecs)
Q_IMPORT_PLUGIN(qtaccessiblewidgets)
#else
#if QT_VERSION < 0x050400
Q_IMPORT_PLUGIN(AccessibleFactory)
#endif
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#endif
#endif
#endif

#if QT_VERSION < 0x050000
#include <QTextCodec>
#endif

// Need a global reference for the notifications to find the GUI
static BitcoinGUI *guiref;
static QSplashScreen *splashref;

static void ThreadSafeMessageBox(const std::string& message, const std::string& caption, int style)
{
    // Message from network thread
    if(guiref)
    {
        bool modal = (style & CClientUIInterface::MODAL);
        // in case of modal message, use blocking connection to wait for user to click OK
        QMetaObject::invokeMethod(guiref, "error",
                                   modal ? GUIUtil::blockingGUIThreadConnection() : Qt::QueuedConnection,
                                   Q_ARG(QString, QString::fromStdString(caption)),
                                   Q_ARG(QString, QString::fromStdString(message)),
                                   Q_ARG(bool, modal));
    }
    else
    {
        printf("%s: %s\n", caption.c_str(), message.c_str());
        fprintf(stderr, "%s: %s\n", caption.c_str(), message.c_str());
    }
}

static bool ThreadSafeAskFee(int64_t nFeeRequired, const std::string& strCaption)
{
    if(!guiref)
        return false;
    if(nFeeRequired < MIN_TX_FEE || nFeeRequired <= nTransactionFee || fDaemon)
        return true;
    bool payFee = false;

    QMetaObject::invokeMethod(guiref, "askFee", GUIUtil::blockingGUIThreadConnection(),
                               Q_ARG(qint64, nFeeRequired),
                               Q_ARG(bool*, &payFee));

    return payFee;
}

static void ThreadSafeHandleURI(const std::string& strURI)
{
    if(!guiref)
        return;

    QMetaObject::invokeMethod(guiref, "handleURI", GUIUtil::blockingGUIThreadConnection(),
                               Q_ARG(QString, QString::fromStdString(strURI)));
}

static void QueueShutdown()
{
    QMetaObject::invokeMethod(QCoreApplication::instance(), "quit", Qt::QueuedConnection);
}

static void InitMessage(const std::string &message)
{
    LogPrintf("init message: %s\n", message);
}

/*
   Translate string to current locale using Qt.
 */
static std::string Translate(const char* psz)
{
    return QCoreApplication::translate("bitcoin-core", psz).toStdString();
}

/** Set up translations */
static void initTranslations(QTranslator& qtTranslatorBase, QTranslator& qtTranslator, QTranslator& translatorBase, QTranslator& translator)
{
    // Remove old translators
    QApplication::removeTranslator(&qtTranslatorBase);
    QApplication::removeTranslator(&qtTranslator);
    QApplication::removeTranslator(&translatorBase);
    QApplication::removeTranslator(&translator);

    // Get desired locale (e.g. "de_DE") from command line or use system locale
    QString lang_territory = QString::fromStdString(GetArg("-lang", QLocale::system().name().toStdString()));

    // Convert to "de" only by truncating "_DE"
    QString lang = lang_territory;
    lang.truncate(lang_territory.lastIndexOf('_'));

    // Load language files for configured locale:
    // - First load the translator for the base language, without territory
    // - Then load the more specific locale translator

    // Load e.g. qt_de.qm
    if (qtTranslatorBase.load("qt_" + lang, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslatorBase);

    // Load e.g. qt_de_DE.qm
    if (qtTranslator.load("qt_" + lang_territory, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);

    // Load e.g. bitcoin_de.qm (shortcut "de" needs to be defined in bitcoin.qrc)
    if (translatorBase.load(lang, ":/translations/"))
        QApplication::installTranslator(&translatorBase);

    // Load e.g. bitcoin_de_DE.qm (shortcut "de_DE" needs to be defined in bitcoin.qrc)
    if (translator.load(lang_territory, ":/translations/"))
        QApplication::installTranslator(&translator);
}


class BitcoinCore: public QObject
{
    Q_OBJECT
public:
    explicit BitcoinCore();

public Q_SLOTS:
    void initialize();
    void shutdown();
    void restart(QStringList args);

Q_SIGNALS:
    void initializeResult(int retval);
    void shutdownResult(int retval);
    void runawayException(const QString &message);

private:
    boost::thread_group threadGroup;

    /// Flag indicating a restart
    bool execute_restart;

    /// Pass fatal exception message to UI thread
    void handleRunawayException(const std::exception *e);
};

void handleRunawayException(const std::exception *e)
{
    PrintExceptionContinue(e, "Runaway exception");
    QMessageBox::critical(0, "Runaway exception", BitcoinGUI::tr("A fatal error occurred. Neutron can no longer continue safely and will quit.") + QString("\n\n") + QString::fromStdString(strMiscWarning));
    exit(1);
}

void BitcoinCore::handleRunawayException(const std::exception *e)
{
    PrintExceptionContinue(e, "Runaway exception");
    Q_EMIT runawayException(QString::fromStdString(strMiscWarning));
}

void BitcoinCore::initialize()
{
    execute_restart = true;

    try
    {
        qDebug() << __func__ << ": Running AppInit2 in thread";
        int rv = AppInit2(threadGroup);
        Q_EMIT initializeResult(rv);
    } catch (const std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(NULL);
    }
}

void BitcoinCore::restart(QStringList args)
{
    if(execute_restart) { // Only restart 1x, no matter how often a user clicks on a restart-button
        execute_restart = false;
        try
        {
            qDebug() << __func__ << ": Running Restart in thread";
            threadGroup.interrupt_all();
            threadGroup.join_all();
            PrepareShutdown();
            qDebug() << __func__ << ": Shutdown finished";
            Q_EMIT shutdownResult(1);
            // CExplicitNetCleanup::callCleanup();
            QProcess::startDetached(QApplication::applicationFilePath(), args);
            qDebug() << __func__ << ": Restart initiated...";
            QApplication::quit();
        } catch (std::exception& e) {
            handleRunawayException(&e);
        } catch (...) {
            handleRunawayException(NULL);
        }
    }
}

void BitcoinCore::shutdown()
{
    try
    {
        qDebug() << __func__ << ": Running Shutdown in thread";
        Interrupt(threadGroup);
        threadGroup.join_all();
        Shutdown();
        qDebug() << __func__ << ": Shutdown finished";
        Q_EMIT shutdownResult(1);
    } catch (const std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(NULL);
    }
}

/** Main Neutron application object */
class BitcoinApplication : public QApplication
{
    Q_OBJECT

public:
    explicit BitcoinApplication(int& argc, char** argv);
    ~BitcoinApplication();

    /// Create main window
    void createWindow(const NetworkStyle *networkStyle);
    /// Create splash screen
    void createSplashScreen();

    /// Request core initialization
    void requestInitialize();
    /// Request core shutdown
    void requestShutdown();

    /// Get process return value
    int getReturnValue() { return returnValue; }

    /// Get window identifier of QMainWindow (BitcoinGUI)
    WId getMainWinId() const;

    // TODO: eventually make private...
    BitcoinGUI *window; // TODO: eventually make private...
    // TODO: eventually make private...

public Q_SLOTS:
    /// Handle runaway exceptions. Shows a message box with the problem and quits the program.
    void handleRunawayException(const QString &message);

Q_SIGNALS:
    void requestedInitialize();
    void requestedShutdown();
    void stopThread();
    void splashFinished(QWidget* window);

private:
    QThread *coreThread;
    // OptionsModel *optionsModel;
    QTimer *pollShutdownTimer;
    int returnValue;

    void startThread();
};

#include "bitcoin.moc"

BitcoinCore::BitcoinCore():
    QObject()
{
}

BitcoinApplication::BitcoinApplication(int &argc, char **argv) :
    QApplication(argc, argv),
    window(0),
    coreThread(0),
    pollShutdownTimer(0),
    returnValue(0)
{
    LogPrintf("Starting application\n");
    setQuitOnLastWindowClosed(false);
    startThread();
}

BitcoinApplication::~BitcoinApplication()
{
    LogPrintf("Stopping application\n");

    if(coreThread)
    {
        qDebug() << __func__ << ": Stopping thread";
        Q_EMIT stopThread();
        coreThread->wait();
        qDebug() << __func__ << ": Stopped thread";
    }

    delete window;
    window = 0;
#ifdef ENABLE_WALLET
    // delete paymentServer;
    // paymentServer = 0;
#endif
    // delete optionsModel;
    // optionsModel = 0;
    // delete platformStyle;
    // platformStyle = 0;
}

void BitcoinApplication::createWindow(const NetworkStyle *networkStyle)
{
    window = new BitcoinGUI();

    pollShutdownTimer = new QTimer(window);
    connect(pollShutdownTimer, SIGNAL(timeout()), window, SLOT(detectShutdown()));
    pollShutdownTimer->start(200);
}

void BitcoinApplication::createSplashScreen()
{
    SplashScreen *splash = new SplashScreen(QPixmap(":/images/splash"), 0);
    // We don't hold a direct pointer to the splash screen after creation, so use
    // Qt::WA_DeleteOnClose to make sure that the window will be deleted eventually.
    splash->setAttribute(Qt::WA_DeleteOnClose);
    splash->show();
    connect(this, SIGNAL(splashFinished(QWidget*)), splash, SLOT(slotFinish(QWidget*)));

    // TODO: eventually remove this pointer to the splash screen
    splashref = splash;
}

void BitcoinApplication::startThread()
{
    // coreThread = new QThread(this);

    // /*  communication to and from thread */
    // connect(this, SIGNAL(stopThread()), coreThread, SLOT(quit()));

    // coreThread->start();

    // connect(this, SIGNAL(requestedInitialize()), executor, SLOT(initialize()));
}

void BitcoinApplication::requestInitialize()
{
    qDebug() << __func__ << ": Requesting initialize";
    startThread();
    Q_EMIT requestedInitialize();
}

void BitcoinApplication::requestShutdown()
{
    // // Show a simple window indicating shutdown status
    // // Do this first as some of the steps may take some time below,
    // // for example the RPC console may still be executing a command.
    // shutdownWindow.reset(ShutdownWindow::showShutdownWindow(window));

    qDebug() << __func__ << ": Requesting shutdown";
    // startThread();
    // window->hide();
    // window->setClientModel(0);
    pollShutdownTimer->stop();

    // delete clientModel;
    // clientModel = 0;

    // Request shutdown from core thread
    Q_EMIT requestedShutdown();
}

void BitcoinApplication::handleRunawayException(const QString &message)
{
    QMessageBox::critical(0, "Runaway exception", BitcoinGUI::tr("A fatal error occurred. Dash Core can no longer continue safely and will quit.") + QString("\n\n") + message);
    ::exit(EXIT_FAILURE);
}

WId BitcoinApplication::getMainWinId() const
{
    if (!window)
        return 0;

    return window->winId();
}

#ifndef BITCOIN_QT_TEST
int main(int argc, char *argv[])
{
    boost::thread_group threadGroup;

    SetupEnvironment();

    /// 1. Parse command-line options. These take precedence over anything else.
    // Command-line options take precedence:
    ParseParameters(argc, argv);

    // Do not refer to data directory yet, this can be overridden by Intro::pickDataDirectory

    /// 2. Basic Qt initialization (not dependent on parameters or configuration)
#if QT_VERSION < 0x050000
    // Internal string conversion is all UTF-8
    QTextCodec::setCodecForTr(QTextCodec::codecForName("UTF-8"));
    QTextCodec::setCodecForCStrings(QTextCodec::codecForTr());
#endif

    Q_INIT_RESOURCE(bitcoin);

    // TODO: probably need to fix OptionsModel before activating this
    // QApplication app(argc, argv);
    BitcoinApplication app(argc, argv);

    // Install global event filter that makes sure that long tooltips can be word-wrapped
    app.installEventFilter(new GUIUtil::ToolTipToRichTextFilter(TOOLTIP_WRAP_THRESHOLD, &app));

    // Command-line options take precedence:
    ParseParameters(argc, argv);

    /// 3. Application identification
    // must be set before OptionsModel is initialized or translations are loaded,
    // as it is used to locate QSettings
    app.setOrganizationName("Neutron");
    app.setOrganizationDomain("neutroncoin.com");
    if(GetBoolArg("-testnet")) // Separate UI settings for testnet
        app.setApplicationName("Neutron-Qt-testnet");
    else
        app.setApplicationName("Neutron-Qt");

    // ... then GUI settings:
    OptionsModel optionsModel;

    /// 4. Initialization of translations, so that intro dialog is in user's language
    // Now that QSettings are accessible, initialize translations
    QTranslator qtTranslatorBase, qtTranslator, translatorBase, translator;
    initTranslations(qtTranslatorBase, qtTranslator, translatorBase, translator);
    uiInterface.Translate.connect(Translate);

    // Show help message immediately after parsing command-line options (for "-lang") and setting locale,
    // but before showing splash screen.
    if (mapArgs.count("-?") || mapArgs.count("--help"))
    {
        GUIUtil::HelpMessageBox help;
        help.showOrPrint();
        return 1;
    }

    // TODO: NTRN - do we want to implement this?
    // /// 5. Now that settings and translations are available, ask user for data directory
    // // User language is set up: pick a data directory
    // Intro::pickDataDirectory();

    /// 6. Determine availability of data directory and parse neutron.conf
    /// - Do not call GetDataDir(true) before this step finishes
    if (!boost::filesystem::is_directory(GetDataDir(false)))
    {
        // This message can not be translated, as translation is not initialized yet
        // (which not yet possible because lang=XX can be overridden in bitcoin.conf in the data directory)
        QMessageBox::critical(0, "Neutron",
                              QString("Error: Specified data directory \"%1\" does not exist.").arg(QString::fromStdString(mapArgs["-datadir"])));
        return 1;
    }
    try {
        ReadConfigFile(mapArgs, mapMultiArgs);
    } catch (std::exception& e) {
        QMessageBox::critical(0, "Neutron",
            QObject::tr("Error: Cannot parse configuration file: %1. Only use key=value syntax.").arg(e.what()));
        return 0;
    }

    // Subscribe to global signals from core
    uiInterface.ThreadSafeMessageBox.connect(ThreadSafeMessageBox);
    uiInterface.ThreadSafeAskFee.connect(ThreadSafeAskFee);
    uiInterface.ThreadSafeHandleURI.connect(ThreadSafeHandleURI);
    uiInterface.QueueShutdown.connect(QueueShutdown);
    uiInterface.Translate.connect(Translate);
    uiInterface.InitMessage.connect(InitMessage);

    if (GetBoolArg("-splash", true) && !GetBoolArg("-min")) {
        app.createSplashScreen();
    }

    app.processEvents();

    app.setQuitOnLastWindowClosed(false);

    try
    {
        // Regenerate startup link, to fix links to old versions
        if (GUIUtil::GetStartOnSystemStartup())
            GUIUtil::SetStartOnSystemStartup(true);

        BitcoinGUI window;
        guiref = &window;
        // guiref = app.window;
        if(AppInit2(threadGroup))
        {
            {
                // Put this in a block, so that the Model objects are cleaned up before
                // calling Shutdown().

                if (splashref)
                    splashref->finish(&window);

                ClientModel clientModel(&optionsModel);
                WalletModel walletModel(pwalletMain, &optionsModel);

                window.setClientModel(&clientModel);
                window.setWalletModel(&walletModel);

                // If -min option passed, start window minimized.
                if(GetBoolArg("-min"))
                {
                    window.showMinimized();
                }
                else
                {
                    window.show();
                }

                app.exec();

                window.hide();
                window.setClientModel(0);
                window.setWalletModel(0);
                guiref = 0;
            }
            // Shutdown the core and its threads, but don't exit Bitcoin-Qt here
            Shutdown();
        }
        else
        {
            return 1;
        }
    } catch (std::exception& e) {
        PrintExceptionContinue(&e, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(strMiscWarning));
    } catch (...) {
        PrintExceptionContinue(NULL, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(strMiscWarning));
    }
    // old // return 0;
    return app.getReturnValue();
}
#endif // BITCOIN_QT_TEST
