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

package mcp

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"

	"github.com/redis/go-redis/v9"
)

// These tests require MCP_AUTH_TOKEN, MCP_MEMORY_USER_ID and a reachable
// TEST_REDIS_URL to be set (the config package reads them at init). They are
// skipped otherwise.
func testClient(t *testing.T) *redis.Client {
	url := os.Getenv("TEST_REDIS_URL")
	if url == "" || !Enabled() {
		t.Skip("set TEST_REDIS_URL, MCP_AUTH_TOKEN and MCP_MEMORY_USER_ID to run")
	}
	opt, err := redis.ParseURL(url)
	if err != nil {
		t.Fatalf("bad TEST_REDIS_URL: %v", err)
	}
	return redis.NewClient(opt)
}

func call(t *testing.T, srv *httptest.Server, body string) map[string]any {
	req, _ := http.NewRequest(http.MethodPost, srv.URL, bytes.NewBufferString(body))
	req.Header.Set("Authorization", "Bearer "+os.Getenv("MCP_AUTH_TOKEN"))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("request failed: %v", err)
	}
	defer resp.Body.Close()
	var out map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&out)
	return out
}

func TestMemoryRoundTrip(t *testing.T) {
	rc := testClient(t)
	srv := httptest.NewServer(NewHandler(rc))
	defer srv.Close()

	// Unauthorized.
	req, _ := http.NewRequest(http.MethodPost, srv.URL, strings.NewReader("{}"))
	resp, _ := http.DefaultClient.Do(req)
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("expected 401 without token, got %d", resp.StatusCode)
	}

	// initialize
	init := call(t, srv, `{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}`)
	if init["result"] == nil {
		t.Fatalf("initialize returned no result: %v", init)
	}

	// tools/list
	list := call(t, srv, `{"jsonrpc":"2.0","id":2,"method":"tools/list"}`)
	tools := list["result"].(map[string]any)["tools"].([]any)
	if len(tools) != 3 {
		t.Fatalf("expected 3 tools, got %d", len(tools))
	}

	// remember
	rem := call(t, srv, `{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"remember","arguments":{"text":"Test fact.","key":"test-key"}}}`)
	if rem["result"] == nil {
		t.Fatalf("remember failed: %v", rem)
	}

	// list_memories should contain it
	ls := call(t, srv, `{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"list_memories"}}`)
	text := ls["result"].(map[string]any)["content"].([]any)[0].(map[string]any)["text"].(string)
	if !strings.Contains(text, "test-key: Test fact.") {
		t.Fatalf("list_memories missing entry: %q", text)
	}

	// forget
	call(t, srv, `{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"forget","arguments":{"key":"test-key"}}}`)
	ls2 := call(t, srv, `{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"list_memories"}}`)
	text2 := ls2["result"].(map[string]any)["content"].([]any)[0].(map[string]any)["text"].(string)
	if strings.Contains(text2, "test-key") {
		t.Fatalf("entry survived forget: %q", text2)
	}
}
