/**
 * Copyright 2025 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

var location = require('./location');
var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');
var customConfigFunction = require('./custom_config');
var config = require('./config');
var reminders = require('./reminders');
var feedback = require('./lib/feedback');
var package_json = require('package.json');


var clay = new Clay(clayConfig, customConfigFunction);

function main() {
    location.update();
    Pebble.addEventListener('appmessage', handleAppMessage);
}

function handleAppMessage(e) {
    console.log("Inbound app message!");
    console.log(JSON.stringify(e));
    var data = e.payload;

    if (reminders.handleReminderMessage(data)) {
        return;
    }

    if ('LOCATION_ENABLED' in data) {
        config.setSetting("LOCATION_ENABLED", !!data.LOCATION_ENABLED);
        console.log("Location enabled: " + config.isLocationEnabled());
        // We need to confirm that we received this for the watch to proceed.
        Pebble.sendAppMessage({
            LOCATION_ENABLED: data.LOCATION_ENABLED,
        });
    }
    if ('FEEDBACK_TEXT' in data) {
        console.log("Handling feedback...");
        feedback.handleFeedbackRequest(data);
    }
    if ('REPORT_THREAD_UUID' in data) {
        console.log("Handling report...");
        feedback.handleReportRequest(data);
    }
}

function doCobbleWarning() {
    if (window.cobble) {
        console.log("WARNING: Running Bobby on Cobble is not supported, and has multiple known issues.");
        Pebble.sendAppMessage({COBBLE_WARNING: 1});
    }
}

Pebble.addEventListener("ready",
    function(e) {
        // This happens before anything else because I don't trust Cobble to get through the normal flow,
        // given how many things bizarrely don't work.
        doCobbleWarning();
        console.log("Bobby " + package_json['version']);
        if (Pebble.platform === 'pypkjs') {
            console.log("Entering emulator mode.");
            var emulator_main = require('./emulator/emulator_main');
            emulator_main.main();
            return;
        }
        console.log("Entering real mode.");
        main();
    }
);
