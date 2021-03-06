/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Krzesimir Nowak <krzesimir@endocode.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <QAbstractButton>
#include <QtCore>
#include <QProcess>
#include <QMessageBox>
#include <QDesktopServices>

#include "wizard/owncloudwizardcommon.h"
#include "wizard/owncloudwizard.h"
#include "mirall/owncloudsetupwizard.h"
#include "mirall/mirallconfigfile.h"
#include "mirall/owncloudinfo.h"
#include "mirall/folderman.h"
#include "mirall/utility.h"
#include "mirall/mirallaccessmanager.h"
#include "creds/abstractcredentials.h"
#include "creds/dummycredentials.h"

namespace Mirall {

OwncloudSetupWizard::OwncloudSetupWizard(QObject* parent) :
    QObject( parent ),
    _ocWizard(new OwncloudWizard),
    _mkdirRequestReply(),
    _checkInstallationRequest(),
    _checkRemoteFolderRequest(),
    _configHandle(),
    _remoteFolder()
{
    connect( _ocWizard, SIGNAL(determineAuthType(const QString&)),
             this, SLOT(slotDetermineAuthType(const QString&)));
    connect( _ocWizard, SIGNAL(connectToOCUrl( const QString& ) ),
             this, SLOT(slotConnectToOCUrl( const QString& )));
    connect( _ocWizard, SIGNAL(createLocalAndRemoteFolders(QString, QString)),
             this, SLOT(slotCreateLocalAndRemoteFolders(QString, QString)));
    /* basicSetupFinished might be called from a reply from the network.
       slotAssistantFinished might destroy the temporary QNetworkAccessManager.
       Therefore Qt::QueuedConnection is required */
    connect( _ocWizard, SIGNAL(basicSetupFinished(int)),
             this, SLOT(slotAssistantFinished(int)), Qt::QueuedConnection);
    connect( _ocWizard, SIGNAL(clearPendingRequests()),
             this, SLOT(slotClearPendingRequests()));
}

OwncloudSetupWizard::~OwncloudSetupWizard()
{
    _ocWizard->deleteLater();
}

void OwncloudSetupWizard::runWizard(QObject* obj, const char* amember, QWidget *parent)
{
    OwncloudSetupWizard *wiz = new OwncloudSetupWizard(parent);
    connect( wiz, SIGNAL(ownCloudWizardDone(int)), obj, amember);
    FolderMan::instance()->setSyncEnabled(false);
    wiz->startWizard();
}

void OwncloudSetupWizard::startWizard()
{
    // Set useful default values.
    MirallConfigFile cfgFile;
    // Fill the entry fields with existing values.
    QString url = cfgFile.ownCloudUrl();
    //QString user = cfgFile.ownCloudUser();
    bool configExists = !( url.isEmpty()/* || user.isEmpty()*/ );
    _ocWizard->setConfigExists( configExists );

    if( !url.isEmpty() ) {
        _ocWizard->setOCUrl( url );
    }

    _remoteFolder = Theme::instance()->defaultServerFolder();
    // remoteFolder may be empty, which means /

    QString localFolder = Theme::instance()->defaultClientFolder();

    // if its a relative path, prepend with users home dir, otherwise use as absolute path
    if( !QDir(localFolder).isAbsolute() ) {
        localFolder = QDir::homePath() + QDir::separator() + localFolder;
    }
    _ocWizard->setProperty("localFolder", localFolder);
    _ocWizard->setRemoteFolder(_remoteFolder);

    _ocWizard->setStartId(WizardCommon::Page_ServerSetup);

    _ocWizard->restart();

    // settings re-initialized in initPage must be set here after restart
    _ocWizard->setMultipleFoldersExist(FolderMan::instance()->map().count() > 1);

    _ocWizard->open();
    _ocWizard->raise();
}

void OwncloudSetupWizard::slotDetermineAuthType(const QString& serverUrl)
{
    QString url(serverUrl);
    qDebug() << "Connect to url: " << url;
    _ocWizard->setField(QLatin1String("OCUrl"), url );
    _ocWizard->appendToConfigurationLog(tr("Trying to connect to %1 at %2 to determine authentication type...")
                                        .arg( Theme::instance()->appNameGUI() ).arg(url) );
    // write a temporary config.
    QDateTime now = QDateTime::currentDateTime();

    // remove a possibly existing custom config.
    if( ! _configHandle.isEmpty() ) {
        // remove the old config file.
        MirallConfigFile oldConfig( _configHandle );
        oldConfig.cleanupCustomConfig();
    }

    _configHandle = now.toString(QLatin1String("MMddyyhhmmss"));

    MirallConfigFile cfgFile( _configHandle, true );
    if( url.isEmpty() ) return;
    if( !( url.startsWith(QLatin1String("https://")) || url.startsWith(QLatin1String("http://"))) ) {
        qDebug() << "url does not start with a valid protocol, assuming https.";
        url.prepend(QLatin1String("https://"));
        // FIXME: give a hint about the auto completion
        _ocWizard->setOCUrl(url);
    }
    cfgFile.writeOwncloudConfig( Theme::instance()->appName(),
                                 url,
                                 new DummyCredentials);

    ownCloudInfo* info = ownCloudInfo::instance();
    info->setCustomConfigHandle( _configHandle );
    if( info->isConfigured() ) {
        // reset the SSL Untrust flag to let the SSL dialog appear again.
        info->resetSSLUntrust();
        connect(info, SIGNAL(ownCloudInfoFound(QString,QString,QString,QString)),
                SLOT(slotOwnCloudFoundAuth(QString,QString,QString,QString)));
        connect(info, SIGNAL(noOwncloudFound(QNetworkReply*)),
                SLOT(slotNoOwnCloudFoundAuth(QNetworkReply*)));
        _checkInstallationRequest = info->checkInstallation();
    } else {
        qDebug() << "   ownCloud seems not to be configured, can not start test connect.";
    }
}

void OwncloudSetupWizard::slotOwnCloudFoundAuth( const QString& url, const QString& infoString, const QString& version, const QString& )
{
    disconnect(ownCloudInfo::instance(), SIGNAL(ownCloudInfoFound(QString,QString,QString,QString)),
               this, SLOT(slotOwnCloudFoundAuth(QString,QString,QString,QString)));
    disconnect(ownCloudInfo::instance(), SIGNAL(noOwncloudFound(QNetworkReply*)),
               this, SLOT(slotNoOwnCloudFoundAuth(QNetworkReply*)));

    _ocWizard->appendToConfigurationLog(tr("<font color=\"green\">Successfully connected to %1: %2 version %3 (%4)</font><br/><br/>")
                                    .arg( url ).arg(Theme::instance()->appNameGUI()).arg(infoString).arg(version));

    MirallAccessManager* nm = new MirallAccessManager(this);
    // TODO: We should get this path from owncloud info.
    QNetworkReply* reply = nm->get (QNetworkRequest (url + "/remote.php/webdav/"));

    connect (reply, SIGNAL(finished()),
             this, SLOT(slotAuthCheckReplyFinished()));

    nm->setProperty ("mirallRedirs", QVariant (0));
}

void OwncloudSetupWizard::slotAuthCheckReplyFinished()
{
    QNetworkReply* reply = qobject_cast< QNetworkReply* > (sender ());
    QUrl redirection = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    QNetworkAccessManager* nm = reply->manager ();
    const int redirCount = nm->property ("mirallRedirs").toInt();

    if (redirCount > 10) {
        redirection.clear ();
    }

    disconnect (reply, SIGNAL(finished()),
                this, SLOT(slotAuthCheckReplyFinished()));
    if ((reply->error () == QNetworkReply::AuthenticationRequiredError) || redirection.isEmpty()) {
        reply->deleteLater();
        nm->deleteLater();
        _ocWizard->setAuthType (WizardCommon::HttpCreds);
    } else if (redirection.toString().endsWith ("/remote.php/webdav/")) {
        QNetworkReply* newReply = nm->get (QNetworkRequest(redirection));

        connect (newReply, SIGNAL(error(QNetworkReply::NetworkError)),
                 this, SLOT(slotAuthCheckReplyError(QNetworkReply::NetworkError)));
        connect (newReply, SIGNAL(finished()),
                 this, SLOT(slotAuthCheckReplyFinished(QNetworkReply::NetworkError)));
        reply->deleteLater();

        nm->setProperty ("mirallRedirs", QVariant(redirCount + 1));
    } else {
        QRegExp shibbolethyWords ("SAML|wayf");

        shibbolethyWords.setCaseSensitivity (Qt::CaseInsensitive);
        if (redirection.toString ().contains (shibbolethyWords)) {
            _ocWizard->setAuthType(WizardCommon::Shibboleth);
        } else {
            // TODO: Send an error.
            // eh?
            _ocWizard->setAuthType (WizardCommon::HttpCreds);
        }
        reply->deleteLater();
        nm->deleteLater();
    }
}

void OwncloudSetupWizard::slotNoOwnCloudFoundAuth( QNetworkReply *err )
{
    disconnect(ownCloudInfo::instance(), SIGNAL(ownCloudInfoFound(QString,QString,QString,QString)),
               this, SLOT(slotOwnCloudFound(QString,QString,QString,QString)));
    disconnect(ownCloudInfo::instance(), SIGNAL(noOwncloudFound(QNetworkReply*)),
               this, SLOT(slotNoOwnCloudFound(QNetworkReply*)));

    _ocWizard->displayError(tr("Failed to connect to %1:<br/>%2").
                            arg(Theme::instance()->appNameGUI()).arg(err->errorString()));

    // remove the config file again
    MirallConfigFile cfgFile( _configHandle );
    cfgFile.cleanupCustomConfig();
}

void OwncloudSetupWizard::slotConnectToOCUrl( const QString& url )
{
  qDebug() << "Connect to url: " << url;
  _ocWizard->setField(QLatin1String("OCUrl"), url );
  _ocWizard->appendToConfigurationLog(tr("Trying to connect to %1 at %2...")
                                  .arg( Theme::instance()->appNameGUI() ).arg(url) );
  testOwnCloudConnect();
}

void OwncloudSetupWizard::testOwnCloudConnect()
{
    // write a temporary config.
    QDateTime now = QDateTime::currentDateTime();

    if( _configHandle.isEmpty() ) {
        _configHandle = now.toString(QLatin1String("MMddyyhhmmss"));
    }

    MirallConfigFile cfgFile( _configHandle, true );
    QString url = _ocWizard->field(QLatin1String("OCUrl")).toString();
    if( url.isEmpty() ) return;
    if( !( url.startsWith(QLatin1String("https://")) || url.startsWith(QLatin1String("http://"))) ) {
        qDebug() << "url does not start with a valid protocol, assuming https.";
        url.prepend(QLatin1String("https://"));
        // FIXME: give a hint about the auto completion
        _ocWizard->setOCUrl(url);
    }
    cfgFile.writeOwncloudConfig( Theme::instance()->appName(),
                                 url,
                                 _ocWizard->getCredentials());

    ownCloudInfo* info(ownCloudInfo::instance());
    info->setCustomConfigHandle( _configHandle );
    // If there is already a config, take its proxy config.
    if( info->isConfigured() ) {
        MirallConfigFile prevCfg;
        cfgFile.setProxyType( prevCfg.proxyType(), prevCfg.proxyHostName(), prevCfg.proxyPort(),
                              prevCfg.proxyNeedsAuth(), prevCfg.proxyUser(), prevCfg.proxyPassword() );
    }

    connect( info,SIGNAL(ownCloudDirExists(QString,QNetworkReply*)),
             this,SLOT(slotConnectionCheck(QString,QNetworkReply*)));

    qDebug() << "# checking for authentication settings.";
    _checkRemoteFolderRequest = info->getWebDAVPath(_remoteFolder ); // this call needs to be authenticated.
    // continue in slotConnectionCheck
}

void OwncloudSetupWizard::slotConnectionCheck(const QString&, QNetworkReply* reply)
{
    // disconnect from ownCloud Info signals
    disconnect(ownCloudInfo::instance(), SIGNAL(ownCloudDirExists(QString,QNetworkReply*)),
               this, SLOT(slotConnectionCheck(QString,QNetworkReply*)));

    switch (reply->error()) {
    case QNetworkReply::NoError:
    case QNetworkReply::ContentNotFoundError:
        _ocWizard->successfulStep();
        break;

    default:
        _ocWizard->displayError(tr("Error: Wrong credentials."));
        break;
    }
}

void OwncloudSetupWizard::slotCreateLocalAndRemoteFolders(const QString& localFolder, const QString& remoteFolder)
{
    qDebug() << "Setup local sync folder for new oC connection " << localFolder;
    const QDir fi( localFolder );
    // FIXME: Show problems with local folder properly.
    bool localFolderOk = true;

    if( fi.exists() ) {
        // there is an existing local folder. If its non empty, it can only be synced if the
        // ownCloud is newly created.
        _ocWizard->appendToConfigurationLog( tr("Local sync folder %1 already exists, setting it up for sync.<br/><br/>").arg(localFolder));
    } else {
        QString res = tr("Creating local sync folder %1... ").arg(localFolder);
        if( fi.mkpath( localFolder ) ) {
            Utility::setupFavLink( localFolder );
            // FIXME: Create a local sync folder.
            res += tr("ok");
        } else {
            res += tr("failed.");
            qDebug() << "Failed to create " << fi.path();
            localFolderOk = false;
            _ocWizard->displayError(tr("Could not create local folder %1").arg(localFolder));
        }
        _ocWizard->appendToConfigurationLog( res );
    }

    if( localFolderOk ) {
        checkRemoteFolder(remoteFolder);
    }
}

void OwncloudSetupWizard::checkRemoteFolder(const QString& remoteFolder)
{
    ownCloudInfo* info(ownCloudInfo::instance());
    connect( info,SIGNAL(ownCloudDirExists(QString,QNetworkReply*)),
             this,SLOT(slotAuthCheckReply(QString,QNetworkReply*)));

    qDebug() << "# checking for existence of remote folder.";
    info->setCustomConfigHandle(_configHandle);
    _checkRemoteFolderRequest = info->getWebDAVPath(remoteFolder); // this call needs to be authenticated.
    // continue in slotAuthCheckReply
}

void OwncloudSetupWizard::slotAuthCheckReply( const QString&, QNetworkReply *reply )
{
    // disconnect from ownCloud Info signals
    disconnect( ownCloudInfo::instance(),SIGNAL(ownCloudDirExists(QString,QNetworkReply*)),
             this,SLOT(slotAuthCheckReply(QString,QNetworkReply*)));

    bool ok = true;
    QString error;
    QNetworkReply::NetworkError errId = reply->error();

    if( errId == QNetworkReply::NoError ) {
        qDebug() << "******** Remote folder found, all cool!";
    } else if( errId == QNetworkReply::ContentNotFoundError ) {
        if( createRemoteFolder() ) {
            return; // Finish here, the mkdir request will go on.
        } else {
            error = tr("The remote folder could not be accessed!");
            ok = false;
        }
    } else {
        error = tr("Error: %1").arg(reply->errorString());
        ok = false;
    }

    if( !ok ) {
        _ocWizard->displayError(error);
    }

    finalizeSetup( ok );
}

bool OwncloudSetupWizard::createRemoteFolder()
{
    if( _remoteFolder.isEmpty() ) return false;

    _ocWizard->appendToConfigurationLog( tr("creating folder on ownCloud: %1" ).arg( _remoteFolder ));
    ownCloudInfo* info(ownCloudInfo::instance());
    connect(info, SIGNAL(webdavColCreated(QNetworkReply::NetworkError)),
            this, SLOT(slotCreateRemoteFolderFinished(QNetworkReply::NetworkError)));

    _mkdirRequestReply = info->mkdirRequest( _remoteFolder );

    return (_mkdirRequestReply != NULL);
}

void OwncloudSetupWizard::slotCreateRemoteFolderFinished( QNetworkReply::NetworkError error )
{
    qDebug() << "** webdav mkdir request finished " << error;
    disconnect(ownCloudInfo::instance(), SIGNAL(webdavColCreated(QNetworkReply::NetworkError)),
               this, SLOT(slotCreateRemoteFolderFinished(QNetworkReply::NetworkError)));

    bool success = true;

    if( error == QNetworkReply::NoError ) {
        _ocWizard->appendToConfigurationLog( tr("Remote folder %1 created successfully.").arg(_remoteFolder));
    } else if( error == 202 ) {
        _ocWizard->appendToConfigurationLog( tr("The remote folder %1 already exists. Connecting it for syncing.").arg(_remoteFolder));
    } else if( error > 202 && error < 300 ) {
        _ocWizard->displayError( tr("The folder creation resulted in HTTP error code %1").arg((int)error ));

        _ocWizard->appendToConfigurationLog( tr("The folder creation resulted in HTTP error code %1").arg((int)error) );
    } else if( error == QNetworkReply::OperationCanceledError ) {
        _ocWizard->displayError( tr("The remote folder creation failed because the provided credentials "
                                    "are wrong!"
                                    "<br/>Please go back and check your credentials.</p>"));
        _ocWizard->appendToConfigurationLog( tr("<p><font color=\"red\">Remote folder creation failed probably because the provided credentials are wrong.</font>"
                                            "<br/>Please go back and check your credentials.</p>"));
        _remoteFolder.clear();
        success = false;
    } else {
        _ocWizard->appendToConfigurationLog( tr("Remote folder %1 creation failed with error <tt>%2</tt>.").arg(_remoteFolder).arg(error));
        _ocWizard->displayError( tr("Remote folder %1 creation failed with error <tt>%2</tt>.").arg(_remoteFolder).arg(error) );
        _remoteFolder.clear();
        success = false;
    }

    finalizeSetup( success );
}

void OwncloudSetupWizard::finalizeSetup( bool success )
{
    // enable/disable the finish button.
    _ocWizard->enableFinishOnResultWidget(success);

    const QString localFolder = _ocWizard->property("localFolder").toString();
    if( success ) {
        if( !(localFolder.isEmpty() || _remoteFolder.isEmpty() )) {
            _ocWizard->appendToConfigurationLog( tr("A sync connection from %1 to remote directory %2 was set up.")
                                             .arg(localFolder).arg(_remoteFolder));
        }
        _ocWizard->appendToConfigurationLog( QLatin1String(" "));
        _ocWizard->appendToConfigurationLog( QLatin1String("<p><font color=\"green\"><b>")
                                         + tr("Successfully connected to %1!")
                                         .arg(Theme::instance()->appNameGUI())
                                         + QLatin1String("</b></font></p>"));
        _ocWizard->successfulStep();
    } else {
        _ocWizard->appendToConfigurationLog(QLatin1String("<p><font color=\"red\">")
                                        + tr("Connection to %1 could not be established. Please check again.")
                                        .arg(Theme::instance()->appNameGUI())
                                        + QLatin1String("</font></p>"));
    }
}

// Method executed when the user ends the wizard, either with 'accept' or 'reject'.
// accept the custom config to be the main one if Accepted.
void OwncloudSetupWizard::slotAssistantFinished( int result )
{
    MirallConfigFile cfg( _configHandle );
    FolderMan *folderMan = FolderMan::instance();

    if( result == QDialog::Rejected ) {
        // the old config remains valid. Remove the temporary one.
        cfg.cleanupCustomConfig();
        qDebug() << "Rejected the new config, use the old!";
    } else if( result == QDialog::Accepted ) {
        AbstractCredentials* credentials(_ocWizard->getCredentials());

        qDebug() << "Config Changes were accepted!";

        // go through all folders and remove the journals if the server changed.
        MirallConfigFile prevCfg;
        QUrl prevUrl( prevCfg.ownCloudUrl() );
        QUrl newUrl( cfg.ownCloudUrl() );
        AbstractCredentials* oldCredentials(prevCfg.getCredentials());

        bool urlHasChanged = (prevUrl.host() != newUrl.host() ||
                prevUrl.port() != newUrl.port() ||
                prevUrl.path() != newUrl.path());

        // if the user changed, its also a changed url.
        if(credentials->changed(oldCredentials)) {
            urlHasChanged = true;
            qDebug() << "The User has changed, same as url change.";
        }

        const QString localFolder = _ocWizard->localFolder();
        bool acceptCfg = true;

        if( urlHasChanged ) {
            // first terminate sync jobs.
            folderMan->terminateSyncProcess();

            bool startFromScratch = _ocWizard->field( "OCSyncFromScratch" ).toBool();
            if( startFromScratch ) {
                // first try to rename (backup) the current local dir.
                bool renameOk = false;
                while( !renameOk ) {
                    renameOk = folderMan->startFromScratch(localFolder);
                    if( ! renameOk ) {
                        QMessageBox::StandardButton but;
                        but = QMessageBox::question( 0, tr("Folder rename failed"),
                                                     tr("Can't remove and back up the folder because the folder or a file in it is open in another program."
                                                        "Please close the folder or file and hit retry or cancel the setup."), QMessageBox::Retry | QMessageBox::Abort, QMessageBox::Retry);
                        if( but == QMessageBox::Abort ) {
                            renameOk = true;
                            acceptCfg = false;
                        }
                    }
                }
            }
        }

        // Now write the resulting folder definition if folder names are set.
        if( acceptCfg && urlHasChanged ) {
            folderMan->removeAllFolderDefinitions();
            folderMan->addFolderDefinition(Theme::instance()->appName(),
                                             localFolder, _remoteFolder );
            _ocWizard->appendToConfigurationLog(tr("<font color=\"green\"><b>Local sync folder %1 successfully created!</b></font>").arg(localFolder));
        } else {
            // url is unchanged. Only the password was changed.
            if( acceptCfg ) {
                qDebug() << "Only password was changed, no changes to folder configuration.";
            } else {
                qDebug() << "User interrupted change of configuration.";
            }
        }

        // save the user credentials and afterwards clear the cred store.
        if( acceptCfg ) {
            cfg.acceptCustomConfig();
        }
    }

    // clear the custom config handle
    _configHandle.clear();
    ownCloudInfo::instance()->setCustomConfigHandle( QString::null );

    // notify others.
    emit ownCloudWizardDone( result );
}

void OwncloudSetupWizard::slotClearPendingRequests()
{
    qDebug() << "Pending request: " << _mkdirRequestReply;
    if( _mkdirRequestReply && _mkdirRequestReply->isRunning() ) {
        qDebug() << "ABORTing pending mkdir request.";
        _mkdirRequestReply->abort();
    }
    if( _checkInstallationRequest && _checkInstallationRequest->isRunning() ) {
        qDebug() << "ABORTing pending check installation request.";
        _checkInstallationRequest->abort();
    }
    if( _checkRemoteFolderRequest && _checkRemoteFolderRequest->isRunning() ) {
        qDebug() << "ABORTing pending remote folder check request.";
        _checkRemoteFolderRequest->abort();
    }
}

} // ns Mirall
