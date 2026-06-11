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

package persistence

import (
	"context"
	"encoding/json"
	"github.com/google/uuid"
	"github.com/honeycombio/beeline-go"
	"github.com/pebble-dev/bobby-assistant/service/assistant/util"
	"github.com/redis/go-redis/v9"
	"time"
)

// FunctionCall and FunctionResponse are provider-neutral. Their JSON tags
// match the genai types they replaced, so previously stored threads and the
// feedback report template keep working.
type FunctionCall struct {
	ID   string         `json:"id,omitempty"`
	Name string         `json:"name,omitempty"`
	Args map[string]any `json:"args,omitempty"`
}

type FunctionResponse struct {
	ID       string         `json:"id,omitempty"`
	Name     string         `json:"name,omitempty"`
	Response map[string]any `json:"response,omitempty"`
}

type SerializedMessage struct {
	Role             string            `json:"role"`
	Content          string            `json:"content"`
	FunctionCall     *FunctionCall     `json:"functionCall,omitempty"`
	FunctionResponse *FunctionResponse `json:"functionResponse,omitempty"`
}

type StoredContext struct {
	PoiQuery  *util.POIQuery `json:"poiQuery"`
	POIs      []util.POI     `json:"pois"`
	LastRoute map[string]any `json:"lastRoute"`
}

type ThreadContext struct {
	ThreadId       uuid.UUID           `json:"threadId"`
	Messages       []SerializedMessage `json:"messages"`
	ContextStorage StoredContext       `json:"contextStorage"`
}

func NewContext() *ThreadContext {
	return &ThreadContext{}
}

func LoadThread(ctx context.Context, r *redis.Client, id string) (*ThreadContext, error) {
	ctx, span := beeline.StartSpan(ctx, "load_thread")
	defer span.Send()
	j, err := r.Get(ctx, "thread:"+id).Result()
	if err != nil {
		return nil, err
	}
	var threadContext ThreadContext
	if err := json.Unmarshal([]byte(j), &threadContext); err != nil {
		return nil, err
	}
	return &threadContext, nil
}

func StoreThread(ctx context.Context, r *redis.Client, thread *ThreadContext) error {
	ctx, span := beeline.StartSpan(ctx, "store_thread")
	defer span.Send()
	j, err := json.Marshal(thread)
	if err != nil {
		span.AddField("error", err)
		return err
	}
	// 24h (was 10m): conversations should survive a workday, distinct from
	// permanent memories (see the memory package), which never expire.
	r.Set(ctx, "thread:"+thread.ThreadId.String(), j, 24*time.Hour)
	return nil
}
