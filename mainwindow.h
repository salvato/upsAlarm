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

#pragma once

#include <QCoreApplication>
#include <QSettings>
#include <QTimer>
#include <QDateTime>
#include <curl/curl.h>
#include <pigpiod_if2.h> // The header for using GPIO pins on Raspberry

QT_FORWARD_DECLARE_CLASS(QFile)


struct
upload_status {
    QStringList* pPayload;
    int lines_read;
};


class MainWindow : public QCoreApplication
{
    Q_OBJECT

public:
    MainWindow(int &argc, char **argv);
    ~MainWindow();
    int  exec();
    static void sigusr1SignalHandler(int unused);

public:
    QStringList payloadText;
    struct upload_status upload_ctx;

public slots:
    void onTimeToCheckTemperature();
    void onTimeToResendAlarm();
    void restoreSettings();

signals:
    void configurationChanged();

protected:
    size_t payloadSource(void *ptr, size_t size, size_t nmemb, void *userp);
    void buildPayload(QString sSubject, QString sMessage);
    bool logRotate(QString sLogFileName);
    void saveSettings();
    void logMessage(QString sMessage);
    bool sendMail(QString sSubject, QString sMessage);
    bool is18B20connected();
    double readTemperature();

protected:
    CURL* curl;
    CURLcode res;
    struct curl_slist* recipients;

private:
    QFile*           pLogFile;
    QString          sLogFileName;
    QString          sSensorFilePath;
    QSettings        settings;
    QTimer           updateTimer;
    uint32_t         updateInterval;
    QTimer           resendTimer;
    uint32_t         resendInterval;
    QTimer           readTemperatureTimer;
    QDateTime        startTime;
    QDateTime        rotateLogTime;
    QDateTime        currentTime;
    int              gpioHostHandle;
    int              gpioSensorPin;
    bool             b18B20exist;
    bool             bOnAlarm;
    bool             bAlarmMessageSent;
    QString          sTdata;

    QString          sUsername;
    QString          sPassword;
    QString          sMailServer;
    QString          sTo;
    QString          sCc;
    QString          sCc1;
    QString          sMessageText;
    double           dMaxTemperature;
};
