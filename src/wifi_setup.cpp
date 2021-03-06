//
// Created by John Beeler on 6/4/18.
//

#include "wifi_setup.h"


#include "tiltBridge.h"
#include <Arduino.h>
//#include "bridge_lcd.h"
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <WiFiClient.h>


// Since we can't use double reset detection or the "boot" button, we need to leverage the touchscreen to trigger the
// WiFi reset on TFT builds
#ifdef LCD_TFT
#include <XPT2046_Touchscreen.h>
XPT2046_Touchscreen ts(TS_CS);
#endif


bool shouldSaveConfig = false;
uint64_t wifi_reset_pressed_at = 0;

//callback notifying us of the need to save config
// TODO - This can probably be eliminated
void saveConfigCallback() {
//    Serial.println("Should save config");
    shouldSaveConfig = true;
}

// callback to display the WiFi LCD notification
void configModeCallback(WiFiManager *myWiFiManager) {
#ifdef DEBUG_PRINTS
    Serial.println("Entered config mode");
    Serial.println(WiFi.softAPIP());
    Serial.println(myWiFiManager->getConfigPortalSSID());
#endif
    // Assuming WIFI_SETUP_AP_PASS here.
    lcd.display_wifi_connect_screen(myWiFiManager->getConfigPortalSSID(), WIFI_SETUP_AP_PASS);
}

// Not sure if this is sufficient to test for validity
bool isValidmDNSName(String mdns_name) {
    for (std::string::size_type i = 0; i < mdns_name.length(); ++i) {
        // For now, we're just checking that every character in the string is alphanumeric. May need to add more validation here.
        if (!isalnum(mdns_name[i]))
            return false;
    }
    return true;
}

void disconnect_from_wifi_and_restart() {
    WiFi.disconnect(true, true);
    ESP.restart();
}

void init_wifi() {
    WiFiManager wifiManager;  //Local initialization. Once its business is done, there is no need to keep it around
    wifiManager.setDebugOutput(false); // In case we have a serial connection
    wifiManager.setConfigPortalTimeout(5 * 60);  // Setting to 5 mins

    // The main purpose of this is to set a boolean value which will allow us to know we
    // just saved a new configuration (as opposed to rebooting normally)
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.setAPCallback(configModeCallback);

    // The third parameter we're passing here (mdns_id.c_str()) is the default name that will appear on the form.
    // It's nice, but it means the user gets no actual prompt for what they're entering.
    std::string mdns_id = app_config.config["mdnsID"];
    WiFiManagerParameter custom_mdns_name("mdns", "Device (mDNS) Name", mdns_id.c_str(), 20);
    wifiManager.addParameter(&custom_mdns_name);

//    std::string password = app_config.config["password"];
//    WiFiManagerParameter custom_password("password", "TiltBridge Password", password.c_str(), 128);
//    wifiManager.addParameter(&custom_password);


    if(wifiManager.autoConnect(WIFI_SETUP_AP_NAME, WIFI_SETUP_AP_PASS)) {
        // TODO - Determine if we can merge shouldSaveConfig in here
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_AP_STA);
    } else {
        // If we haven't successfully connected to WiFi, just restart & continue to project the configuration AP.
        // Alternatively, we can hang here.
        ESP.restart();
    }

    // Alright. We're theoretically connected here.
    // If we connected, then let's save the mDNS name
    if (shouldSaveConfig) {
        // If the mDNS name is valid, save it.
        if (isValidmDNSName(custom_mdns_name.getValue())) {
            app_config.config["mdnsID"] = custom_mdns_name.getValue();
            mdns_id = app_config.config["mdnsID"];
        } else {
            // If the mDNS name is invalid, reset the WiFi configuration and restart the ESP8266
            // TODO - add an LCD error message here maybe
            disconnect_from_wifi_and_restart();
        }

//        app_config.config["password"] = custom_password.getValue();
        app_config.save();
    }

    if (!MDNS.begin(mdns_id.c_str())) {
//        Serial.println("Error setting up MDNS responder!");
    }

    MDNS.addService("http", "tcp", 80);  // technically we should wait on this, but I'm impatient.
    MDNS.addService("tiltbridge", "tcp", 80);  // for lookups

    // Display a screen so the user can see how to access the Tiltbridge
    String mdns_url = String("http://");
    mdns_url = mdns_url + mdns_id.c_str();
    mdns_url = mdns_url + ".local/";
    String ip_address_url = String("http://");
    ip_address_url = ip_address_url + WiFi.localIP().toString();
    ip_address_url.concat("/");

    lcd.display_wifi_success_screen(mdns_url, ip_address_url);
    delay(1000);
}


#ifndef LCD_TFT
// Use the "boot" button present on most of the OLED boards to reset the WiFi configuration allowing for easy
// transportation between networks
void IRAM_ATTR wifi_reset_pressed() {
    // When the reset button is pressed, just log the time & get back to work
    wifi_reset_pressed_at = xTaskGetTickCount();
}

void initWiFiResetButton() {
    pinMode(WIFI_RESET_BUTTON_GPIO, INPUT_PULLUP);
    attachInterrupt(WIFI_RESET_BUTTON_GPIO, wifi_reset_pressed, RISING);
}

//void disableWiFiResetButton() {
//    detachInterrupt(WIFI_RESET_BUTTON_GPIO);
//}

#else
// If we have LCD_TFT set, we need to build the functions as noop
void initWiFiResetButton() {
    // This is noop for LCD_TFT as we're using the touchscreen instead
}

#endif

void handle_wifi_reset_presses() {
    uint64_t initial_press_at = 0;


#ifdef LCD_TFT
    while (ts.touched())  // Block while the screen is pressed until the user releases
        wifi_reset_pressed_at = xTaskGetTickCount();
#endif


    if(wifi_reset_pressed_at > (xTaskGetTickCount() - WIFI_RESET_DOUBLE_PRESS_TIME) && wifi_reset_pressed_at > WIFI_RESET_DOUBLE_PRESS_TIME) {
        initial_press_at = wifi_reset_pressed_at; // Cache when the button was first pressed
        lcd.display_wifi_reset_screen();
        delay(100); // Give the user a moment to release the screen (doubles as debounce)

        for(TickType_t x = xTaskGetTickCount() + WIFI_RESET_DOUBLE_PRESS_TIME; xTaskGetTickCount() <= x;) {
            delay(1);

#ifdef LCD_TFT
            if(ts.touched() || wifi_reset_pressed_at != initial_press_at)
#else
            if(wifi_reset_pressed_at != initial_press_at)
#endif
            {
                // The user pushed the button a second time & caused a second interrupt. Process the reset.
                disconnect_from_wifi_and_restart();
            }
        }

        // Explicitly clear the screen
        lcd.clear();

//        delay(WIFI_RESET_DOUBLE_PRESS_TIME); // Block while we let the user press a second time
//
//        if(wifi_reset_pressed_at != initial_press_at) {
//            // The user pushed the button a second time & caused a second interrupt. Process the reset.
//            disconnect_from_wifi_and_restart();
//        }
    }
}


void reconnectIfDisconnected() {
    if(WiFiClass::status() !=  WL_CONNECTED) {
        // WiFi is down - Reconnect
        lcd.display_wifi_disconnected_screen();
        WiFi.begin();

        delay(1000);  // Ensuring the "disconnected" screen appears for at least one second

        int WLcount = 0;
        while(WiFiClass::status() != WL_CONNECTED && WLcount < 190)
        {
            delay( 100 );
#ifdef DEBUG_PRINTS
            if(WLcount % 5 == 0)
                Serial.printf(".");
#endif
            ++WLcount;
        }

        if(WiFiClass::status() !=  WL_CONNECTED) {
            // We failed to reconnect.
            lcd.display_wifi_reconnect_failed();
        } else {
            // We reconnected successfully
            lcd.display_logo();
        }

    }
}

