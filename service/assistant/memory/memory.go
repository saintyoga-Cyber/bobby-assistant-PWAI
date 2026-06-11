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

// Package memory stores long-term, per-user memories in a Redis hash
// (memory:<user_id>). Memories are injected into the system prompt on every
// request, so the set is deliberately capped — this is a notebook, not a
// vector store.
package memory

import (
	"context"
	"fmt"
	"sort"

	"github.com/redis/go-redis/v9"
)

const (
	// MaxMemories caps how many entries a user can store.
	MaxMemories = 200
	// MaxTextLength caps a single memory's length.
	MaxTextLength = 1000
	// MaxKeyLength caps a memory key's length.
	MaxKeyLength = 100
)

func redisKey(userId int) string {
	return fmt.Sprintf("memory:%d", userId)
}

// Remember stores (or overwrites) a memory under the given key.
func Remember(ctx context.Context, r *redis.Client, userId int, key, text string) error {
	if len(key) > MaxKeyLength {
		return fmt.Errorf("memory key too long (max %d characters)", MaxKeyLength)
	}
	if len(text) > MaxTextLength {
		return fmt.Errorf("memory text too long (max %d characters)", MaxTextLength)
	}
	count, err := r.HLen(ctx, redisKey(userId)).Result()
	if err != nil {
		return err
	}
	exists, err := r.HExists(ctx, redisKey(userId), key).Result()
	if err != nil {
		return err
	}
	if !exists && count >= MaxMemories {
		return fmt.Errorf("memory is full (%d entries) — forget something first", MaxMemories)
	}
	return r.HSet(ctx, redisKey(userId), key, text).Err()
}

// Forget deletes a memory by key. Returns whether the key existed.
func Forget(ctx context.Context, r *redis.Client, userId int, key string) (bool, error) {
	deleted, err := r.HDel(ctx, redisKey(userId), key).Result()
	if err != nil {
		return false, err
	}
	return deleted > 0, nil
}

// Entry is a single stored memory.
type Entry struct {
	Key  string
	Text string
}

// GetAll returns every memory for the user, sorted by key so the rendered
// system prompt is byte-stable between requests.
func GetAll(ctx context.Context, r *redis.Client, userId int) ([]Entry, error) {
	m, err := r.HGetAll(ctx, redisKey(userId)).Result()
	if err != nil {
		return nil, err
	}
	entries := make([]Entry, 0, len(m))
	for k, v := range m {
		entries = append(entries, Entry{Key: k, Text: v})
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].Key < entries[j].Key })
	return entries, nil
}
