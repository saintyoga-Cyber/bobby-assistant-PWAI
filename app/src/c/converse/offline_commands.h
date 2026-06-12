/*
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

#ifndef OFFLINE_COMMANDS_H
#define OFFLINE_COMMANDS_H

#include "conversation_manager.h"

// Attempts to handle a transcribed prompt entirely on the watch (set/cancel a
// timer or alarm), with no round trip to the service. Returns true if the
// input was a recognised quick command and has been handled (a confirmation
// response has been added to the conversation); returns false if the caller
// should fall back to sending the prompt to the service as usual.
//
// The grammar is deliberately strict: only utterances that are essentially
// just the command match, so compound requests still reach the assistant.
bool offline_commands_try(ConversationManager* manager, const char* input);

#endif
