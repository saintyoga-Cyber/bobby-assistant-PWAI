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
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"

	"github.com/honeycombio/beeline-go"
	"google.golang.org/genai"

	"github.com/pebble-dev/bobby-assistant/service/assistant/config"
	"github.com/pebble-dev/bobby-assistant/service/assistant/quota"
)

type WebSearchInput struct {
	// The question to answer using a live web search.
	Query string `json:"query"`
}

type WebSearchSource struct {
	Title string `json:"title,omitempty"`
	URL   string `json:"url,omitempty"`
}

type WebSearchResponse struct {
	Answer  string            `json:"answer"`
	Sources []WebSearchSource `json:"sources,omitempty"`
}

func init() {
	// The web_search function is backed by Perplexity; without a key it would
	// fail on every call, so don't offer it to the model at all.
	if config.GetConfig().PerplexityKey == "" {
		return
	}
	f := false
	registerFunction(Registration{
		Definition: genai.FunctionDeclaration{
			Name: "web_search",
			Description: "Search the live web and get a sourced answer. Use this for anything after your knowledge cutoff or " +
				"that changes over time: news, current events, sports results, prices, release dates, schedules, or when the " +
				"user asks you to search or look something up. Phrase the query as a complete question.",
			Parameters: &genai.Schema{
				Type:     genai.TypeObject,
				Nullable: &f,
				Properties: map[string]*genai.Schema{
					"query": {
						Type:        genai.TypeString,
						Description: "The question to answer, e.g. 'Who won the 2026 Champions League final?'",
						Nullable:    &f,
					},
				},
				Required: []string{"query"},
			},
		},
		Fn:                        webSearchImpl,
		Thought:                   webSearchThought,
		InputType:                 WebSearchInput{},
		RedactOutputInChatHistory: true,
	})
}

func webSearchThought(args any) string {
	return "Searching the web"
}

type perplexityRequest struct {
	Model    string              `json:"model"`
	Messages []perplexityMessage `json:"messages"`
}

type perplexityMessage struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

type perplexityResponse struct {
	Choices []struct {
		Message struct {
			Content string `json:"content"`
		} `json:"message"`
	} `json:"choices"`
	Citations     []string `json:"citations"`
	SearchResults []struct {
		Title string `json:"title"`
		URL   string `json:"url"`
	} `json:"search_results"`
}

func webSearchImpl(ctx context.Context, qt *quota.Tracker, args any) any {
	ctx, span := beeline.StartSpan(ctx, "web_search")
	defer span.Send()
	arg := args.(*WebSearchInput)
	if arg.Query == "" {
		return Error{"No query provided."}
	}
	if err := qt.ChargeCredits(ctx, quota.WebSearchCredits); err != nil {
		span.AddField("error", err)
		return Error{"quota charge failed: " + err.Error()}
	}

	body, err := json.Marshal(perplexityRequest{
		Model: "sonar",
		Messages: []perplexityMessage{
			{Role: "system", Content: "Answer concisely for display on a smartwatch. Plain text only, no markdown."},
			{Role: "user", Content: arg.Query},
		},
	})
	if err != nil {
		return Error{err.Error()}
	}
	reqCtx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(reqCtx, http.MethodPost, "https://api.perplexity.ai/chat/completions", bytes.NewReader(body))
	if err != nil {
		return Error{err.Error()}
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+config.GetConfig().PerplexityKey)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		span.AddField("error", err)
		return Error{"web search failed: " + err.Error()}
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		errBody, _ := io.ReadAll(io.LimitReader(resp.Body, 1024))
		span.AddField("error", string(errBody))
		return Error{fmt.Sprintf("web search failed with status %d", resp.StatusCode)}
	}
	var parsed perplexityResponse
	if err := json.NewDecoder(resp.Body).Decode(&parsed); err != nil {
		span.AddField("error", err)
		return Error{"web search returned an unparseable response"}
	}
	if len(parsed.Choices) == 0 {
		return Error{"web search returned no answer"}
	}
	result := WebSearchResponse{Answer: parsed.Choices[0].Message.Content}
	for _, sr := range parsed.SearchResults {
		result.Sources = append(result.Sources, WebSearchSource{Title: sr.Title, URL: sr.URL})
	}
	if len(result.Sources) == 0 {
		for _, c := range parsed.Citations {
			result.Sources = append(result.Sources, WebSearchSource{URL: c})
		}
	}
	return result
}
