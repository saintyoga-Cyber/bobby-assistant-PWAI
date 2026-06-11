// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package functions

import (
	"context"
	"fmt"
	"strings"
	"time"

	"github.com/honeycombio/beeline-go"
	"google.golang.org/genai"

	"github.com/pebble-dev/bobby-assistant/service/assistant/memory"
	"github.com/pebble-dev/bobby-assistant/service/assistant/quota"
	"github.com/pebble-dev/bobby-assistant/service/assistant/util/storage"
)

type RememberInput struct {
	// The fact to remember.
	Text string `json:"text"`
	// A short identifier for the memory.
	Key string `json:"key"`
}

type ForgetInput struct {
	// The key of the memory to delete.
	Key string `json:"key"`
}

type MemoryResponse struct {
	Status string `json:"status"`
	Key    string `json:"key,omitempty"`
}

func init() {
	f := false
	registerFunction(Registration{
		Definition: genai.FunctionDeclaration{
			Name: "remember",
			Description: "Store a long-term memory about the user (preferences, facts about them, things they ask you to remember). " +
				"Stored memories are shown to you at the start of every future conversation. " +
				"Use this whenever the user shares something durable about themselves, or explicitly asks you to remember something. " +
				"Reusing an existing key overwrites that memory - do this to update outdated information.",
			Parameters: &genai.Schema{
				Type:     genai.TypeObject,
				Nullable: &f,
				Properties: map[string]*genai.Schema{
					"text": {
						Type:        genai.TypeString,
						Description: "The fact to remember, phrased as a short standalone statement, e.g. 'The user's wife is called Maria.'",
						Nullable:    &f,
					},
					"key": {
						Type:        genai.TypeString,
						Description: "A short kebab-case identifier for this memory, e.g. 'wife-name'. Omit to have one generated.",
					},
				},
				Required: []string{"text"},
			},
		},
		Fn:        rememberImpl,
		Thought:   rememberThought,
		InputType: RememberInput{},
	})
	registerFunction(Registration{
		Definition: genai.FunctionDeclaration{
			Name: "forget",
			Description: "Delete a stored long-term memory about the user by its key. The keys of all stored memories are listed " +
				"in the 'Things you remember about the user' section of your instructions.",
			Parameters: &genai.Schema{
				Type:     genai.TypeObject,
				Nullable: &f,
				Properties: map[string]*genai.Schema{
					"key": {
						Type:        genai.TypeString,
						Description: "The key of the memory to delete.",
						Nullable:    &f,
					},
				},
				Required: []string{"key"},
			},
		},
		Fn:        forgetImpl,
		Thought:   forgetThought,
		InputType: ForgetInput{},
	})
}

func rememberThought(args any) string {
	return "Making a note of that"
}

func forgetThought(args any) string {
	return "Forgetting that"
}

func rememberImpl(ctx context.Context, qt *quota.Tracker, args any) any {
	ctx, span := beeline.StartSpan(ctx, "remember")
	defer span.Send()
	arg := args.(*RememberInput)
	if strings.TrimSpace(arg.Text) == "" {
		return Error{"Nothing to remember: 'text' is empty."}
	}
	key := strings.TrimSpace(arg.Key)
	if key == "" {
		key = fmt.Sprintf("note-%d", time.Now().Unix())
	}
	if err := memory.Remember(ctx, storage.GetRedis(), qt.UserId(), key, arg.Text); err != nil {
		span.AddField("error", err)
		return Error{err.Error()}
	}
	return MemoryResponse{Status: "remembered", Key: key}
}

func forgetImpl(ctx context.Context, qt *quota.Tracker, args any) any {
	ctx, span := beeline.StartSpan(ctx, "forget")
	defer span.Send()
	arg := args.(*ForgetInput)
	existed, err := memory.Forget(ctx, storage.GetRedis(), qt.UserId(), strings.TrimSpace(arg.Key))
	if err != nil {
		span.AddField("error", err)
		return Error{err.Error()}
	}
	if !existed {
		return Error{fmt.Sprintf("No memory with key %q exists.", arg.Key)}
	}
	return MemoryResponse{Status: "forgotten", Key: arg.Key}
}
