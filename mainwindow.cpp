// Copyright (C) 2020  Gabriele Salvato

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.


#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>
#include <syslog.h>
#include <signal.h>

#include "mainwindow.h"
#include "pigpiod_if2.h"


size_t
payloadSource(void *ptr, size_t size, size_t nmemb, void *userp) {
    struct upload_status* upload_ctx = (struct upload_status*)userp;
    if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1))
        return 0;
    if(upload_ctx->lines_read >= upload_ctx->pPayload->count())
        return 0;
    QString sLine = QString("%1\r\n").arg(upload_ctx->pPayload->at(upload_ctx->lines_read));
    size_t len = qMin(size_t(sLine.length()), size*nmemb);
    memcpy(ptr, sLine.toLatin1().constData(), len);
    upload_ctx->lines_read++;
    return len;
}


MainWindow* pThis = nullptr;


MainWindow::MainWindow(int &argc, char **argv)
    : QCoreApplication(argc, argv)
    , recipients(nullptr)
    , pLogFile(nullptr)
{
    pThis = this;
    gpioHostHandle = -1;
    gpioSensorPin  = 23; // BCM 23: pin 16 in the 40 pins GPIO connector
    // DS18B20 connected to BCM  4: pin 7  in the 40 pins GPIO connector
    res            = CURLE_OK;
    bOnAlarm          = false;
    bAlarmMessageSent = false;
    b18B20exist       = false;

    updateInterval = 60*1000;        // 60 sec (in ms)
    resendInterval = 30 * 60 * 1000; // 30 min (in ms)

    // Build the log file pathname
    sLogFileName = QString("UPS-AlarmLog.txt");
    QString sLogDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if(!sLogDir.endsWith(QString("/"))) sLogDir+= QString("/");
    sLogFileName = sLogDir+sLogFileName;

    if(!logRotate(sLogFileName)) { // If unable to open Log File then Log to syslog
        QString sAppName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
        // See the syslog(3) man page
        openlog(sAppName.toLatin1().constData(), LOG_PID, LOG_USER);
    }

    restoreSettings();
}


int
MainWindow::exec() {
    struct sigaction usr1;
    usr1.sa_handler = MainWindow::sigusr1SignalHandler;
    sigemptyset(&usr1.sa_mask);
    usr1.sa_flags = 0;
    if(sigaction(SIGUSR1, &usr1, nullptr) != 0)
        qDebug() << "Unable to handle the SIGUSR1 signal";
    connect(this, SIGNAL(configurationChanged()),
            this, SLOT(restoreSettings()));

    b18B20exist = is18B20connected();

    // Initialize the GPIO handler
    gpioHostHandle = pigpio_start((char*)"localhost", (char*)"8888");
    if(gpioHostHandle >= 0) {
        if(set_mode(gpioHostHandle, gpioSensorPin, PI_INPUT) < 0) {
            logMessage(QString("Unable to initialize GPIO%1 as Output")
                       .arg(gpioSensorPin));
            gpioHostHandle = -1;
        }
        else if(set_pull_up_down(gpioHostHandle, gpioSensorPin, PI_PUD_UP) < 0) {
            logMessage(QString("Unable to set GPIO%1 Pull-Up")
                       .arg(gpioSensorPin));
            gpioHostHandle = -1;
        }
    }
    else {
        logMessage(QString("Unable to initialize the Pi GPIO."));
    }
    connect(&updateTimer, SIGNAL(timeout()),
            this, SLOT(onTimeToCheckTemperature()));
    connect(&resendTimer, SIGNAL(timeout()),
            this, SLOT(onTimeToResendAlarm()));

    startTime = QDateTime::currentDateTime();
    rotateLogTime = startTime;
    // Check the Thermostat Status every minute
    updateTimer.start(updateInterval);

#ifndef QT_DEBUG
    logMessage("System Started");
    if(sendMail("UPS Temperature Alarm System [INFO]",
                "The Alarm System Has Been Restarted"))
        logMessage("UPS Temperature Alarm System [INFO]: Message Sent");
    else
        logMessage("UPS Temperature Alarm System [INFO]: Unable to Send the Message");
#endif
    return QCoreApplication::exec();
}


MainWindow::~MainWindow() {
    logMessage("Switching Off the Program");
    updateTimer.stop();
    readTemperatureTimer.stop();
    resendTimer.stop();

#ifndef QT_DEBUG
    if(sendMail("UPS Temperature Alarm System [INFO]",
                "The Alarm System Has Been Switched Off"))
        logMessage("Message Sent");
    else
        logMessage("Unable to Send the Message");
#endif

    if(gpioHostHandle >= 0)
        pigpio_stop(gpioHostHandle);
    if(pLogFile) {
        if(pLogFile->isOpen()) {
            pLogFile->flush();
        }
        pLogFile->deleteLater();
        pLogFile = nullptr;
    }
    closelog();
}


void
MainWindow::sigusr1SignalHandler(int unused) {
    Q_UNUSED(unused)
    emit pThis->configurationChanged();
}


bool
MainWindow::is18B20connected() {
    bool bFound = false;
    QString s1WireDir = "/sys/bus/w1/devices/";
    QDir dir1Wire(s1WireDir);
    if(dir1Wire.exists()) {
        dir1Wire.setFilter(QDir::Dirs);
        QStringList filter;
        filter.append(QString("10-*"));
        filter.append(QString("28-*"));
        QStringList subDirs = dir1Wire.entryList(filter);

        if(subDirs.count() != 0) {
            for(int i=0; i<subDirs.count(); i++) {
                sSensorFilePath = dir1Wire.absolutePath() +
                                  QString("/") +
                                  subDirs.at(i) +
                                  QString("/w1_slave");
                QFile* pSensorFile = new QFile(sSensorFilePath);
                if(!pSensorFile->open(QIODevice::Text|QIODevice::ReadOnly)) {
                    delete pSensorFile;
                    pSensorFile = nullptr;
                    continue;
                }
                sTdata = QString(pSensorFile->readAll());
                if(sTdata.contains("YES")) {
                    bFound = true;
                    pSensorFile->close();
                    delete pSensorFile;
                    pSensorFile = nullptr;
                    break;
                }
                pSensorFile->close();
                delete pSensorFile;
                pSensorFile = nullptr;
            }
        }
    }
    return bFound;
}


bool
MainWindow::logRotate(QString sLogFileName) {
    // Rotate 5 previous logs, removing the oldest, to avoid data loss
    QFileInfo checkFile(sLogFileName);
    if(checkFile.exists() && checkFile.isFile()) {
#ifdef QT_DEBUG
        qDebug() << "Rotating Log File";
#endif
        QDir renamed;
        renamed.remove(sLogFileName+QString("_4.txt"));
        for(int i=4; i>0; i--) {
            renamed.rename(sLogFileName+QString("_%1.txt").arg(i-1),
                           sLogFileName+QString("_%1.txt").arg(i));
        }
        if(pLogFile) {
            if(pLogFile->isOpen())
                pLogFile->close();
            delete pLogFile;
            pLogFile = nullptr;
        }
        renamed.rename(sLogFileName,
                       sLogFileName+QString("_0.txt"));
    }
    // Open the new log file
    pLogFile = new QFile(sLogFileName);
    if (!pLogFile->open(QIODevice::WriteOnly)) {
        logMessage(QString("Unable to open file %1: %2.")
                   .arg(sLogFileName).arg(pLogFile->errorString()));
        delete pLogFile;
        pLogFile = Q_NULLPTR;
        return false;
    }
    return true;
}

void
MainWindow::logMessage(QString sMessage) {
    QString sDebugMessage = currentTime.currentDateTime().toString() +
            QString(": ") +
            sMessage;
#ifdef QT_DEBUG
    qDebug() << sDebugMessage;
#endif
    if(pLogFile) {
        if(pLogFile->isOpen()) {
            pLogFile->write(sDebugMessage.toLatin1().constData());
            pLogFile->write("\n");
            pLogFile->flush();
        }
        else
            syslog(LOG_ALERT|LOG_USER, "%s", sMessage.toLatin1().constData());
    }
    else
        syslog(LOG_ALERT|LOG_USER, "%s", sMessage.toLatin1().constData());
}


void
MainWindow::restoreSettings() {
    logMessage("Settings Changed");
    sUsername = settings.value("Username:", "upsgenerale").toString();
    sPassword = settings.value("Password:", "").toString();
    sMailServer = settings.value("Mail Server:", "posta.ipcf.cnr.it").toString();
    sTo = settings.value("To:", "").toString();
    sCc = settings.value("Cc:", "").toString();
    sCc1 = settings.value("Cc1:", "").toString();
    dMaxTemperature = settings.value("Alarm Threshold", "28.0").toDouble();
    logMessage(QString("Threshold: %1").arg(dMaxTemperature));
    sMessageText = settings.value("Message to Send:", "").toString();
}


void
MainWindow::buildPayload(QString sSubject, QString sMessage) {
    payloadText.clear();

    payloadText.append(QString("Date: %1")
                       .arg(currentTime.currentDateTime().toString(Qt::RFC2822Date)));
    payloadText.append(QString("To: %1")
                       .arg(sTo));
    payloadText.append(QString("From: %1@%2")
                       .arg(sUsername)
                       .arg(sMailServer));
    if(sCc != QString()) {
        payloadText.append(QString("Cc: <%1>")
                           .arg(sCc));
    }
    if(sCc1 != QString()) {
        payloadText.append(QString("Cc: <%1>")
                           .arg(sCc1));
    }
    QString sMessageId = QString("Message-ID: <%1@ipcf.cnr.it>")
            .arg(currentTime.currentDateTime().toString().replace(QChar(' '), QChar('#')));
    payloadText.append(sMessageId);
    payloadText.append(QString("Subject: %1")
                       .arg(sSubject));
    // empty line to divide headers from body, see RFC5322
    payloadText.append(QString());
    // Body
    payloadText.append(currentTime.currentDateTime().toString());
    QStringList sMessageBody = sMessage.split("\n");
    payloadText.append(sMessageBody);
}


bool
MainWindow::sendMail(QString sSubject, QString sMessage) {
    buildPayload(sSubject, sMessage);
    upload_ctx.lines_read = 0;
    upload_ctx.pPayload = &payloadText;

    curl = curl_easy_init();
    if(curl) {
        QString mailserverURL = QString("smtps://%1@%2")
                .arg(sUsername)
                .arg(sMailServer);
        curl_easy_setopt(curl, CURLOPT_URL, mailserverURL.toLatin1().constData());
//        curl_easy_setopt(curl, CURLOPT_CAINFO, "/home/pi/posta_ipcf_cnr_it.crt");
//        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        QString sMailFrom = QString("<%1@%2>")
                            .arg(sUsername)
                            .arg(sMailServer);
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, sMailFrom.toLatin1().constData());
        curl_easy_setopt(curl, CURLOPT_USERNAME,  sUsername.toLatin1().constData());
        curl_easy_setopt(curl, CURLOPT_PASSWORD,  sPassword.toLatin1().constData());

        recipients = curl_slist_append(recipients, sTo.toLatin1().constData());
        if(sCc != QString()) {
            recipients = curl_slist_append(recipients, (QString("<%1>").arg(sCc)).toLatin1().constData());
        }
        if(sCc1 != QString()) {
            recipients = curl_slist_append(recipients, (QString("<%1>").arg(sCc1)).toLatin1().constData());
        }
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

        curl_easy_setopt(curl, CURLOPT_READFUNCTION, ::payloadSource);
        curl_easy_setopt(curl, CURLOPT_READDATA, &upload_ctx);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
#ifdef QT_DEBUG
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
#endif
        // Send the message
        res = curl_easy_perform(curl);
        if(res != CURLE_OK)
            logMessage(QString("curl_easy_perform() failed: %1")
                       .arg(curl_easy_strerror(res)));

        // Free the list of recipients
        curl_slist_free_all(recipients);
        recipients = nullptr;
        curl_easy_cleanup(curl);
    }
    return (res==CURLE_OK);
}


void
MainWindow::onTimeToCheckTemperature() {
    // Check if it's time (every 7 days) to rotate log:
    if(rotateLogTime.daysTo(QDateTime::currentDateTime()) > 7) {
        logRotate(sLogFileName);
        rotateLogTime = QDateTime::currentDateTime();
    }
    bOnAlarm = (gpio_read(gpioHostHandle, gpioSensorPin) == 0);
    if(b18B20exist) {
        double temperature = readTemperature();
        if(temperature > -300.0) {
            bOnAlarm |= (temperature > dMaxTemperature);
            logMessage(QString("Temperature: %1, %2")
                       .arg(double(startTime.secsTo(QDateTime::currentDateTime())/3600.0))
                       .arg(temperature));
        }
    }
    if(bOnAlarm  && !bAlarmMessageSent) {
        logMessage("TEMPERATURE ALARM !");
        if(sendMail("UPS Temperature Alarm System [ALARM!]",
                    sMessageText))
        {
            bAlarmMessageSent = true;
            logMessage("UPS Temperature Alarm System [ALARM!]: Message Sent");
            // Start a timer to retransmit the alarm message every 30 minutes
            resendTimer.start(resendInterval);
        }
        else
            logMessage("PS Temperature Alarm System [ALARM!]: Unable to Send the Message");
    }
}


void
MainWindow::onTimeToResendAlarm() {
    if(!bOnAlarm) {
        logMessage("Temperature Alarm Ceased");
        if(sendMail("UPS Temperature Alarm System [INFO!]",
                    "Temperature Alarm Ceased"))
            logMessage("UPS Temperature Alarm System [INFO!]: Message Sent");
        else
            logMessage("UPS Temperature Alarm System [INFO!]: Unable to Send the Message");
        resendTimer.stop();
        bAlarmMessageSent = false;
    }
    else { // Still on Alarm !
        logMessage("TEMPERATURE ALARM STILL ON!");
        if(sendMail("UPS Temperature Alarm System [ALARM!]",
                    sMessageText))
            logMessage("UPS Temperature Alarm System [ALARM!]: Message Sent");
        else
            logMessage("UPS Temperature Alarm System [ALARM!]: Unable to Send the Message");
    }
}


// Return the Temperature read from DS18B20 or a value
// lower than -300.0 to signal an erratic sensor
double
MainWindow::readTemperature() {
    double temperature = -999.9;
    if(b18B20exist) {
        QFile SensorFile(sSensorFilePath);
        if(!SensorFile.open(QIODevice::Text|QIODevice::ReadOnly)) {
            b18B20exist = false;
            logMessage("Temperature Sensor NOT Responding !");
            if(sendMail("UPS Temperature Alarm System [WARNING!]",
                        "Temperature Sensor NOT responding !"))
                logMessage("UPS Temperature Alarm System [WARNING!]: Message Sent");
            else
                logMessage("UPS Temperature Alarm System [WARNING!]: Unable to Send the Message");
        }
        else {
            sTdata = QString(SensorFile.readAll());
            if(sTdata.contains("YES")) {
                int iPos = sTdata.indexOf("t=");
                if(iPos > 0) {
                    temperature = double(sTdata.mid(iPos+2).toDouble()/1000.0);
                }
            }
            SensorFile.close();
        }
    }
    return temperature;
}
