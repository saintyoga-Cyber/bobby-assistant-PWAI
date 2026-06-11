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

package assistant

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"regexp"
	"strings"
	"time"

	"github.com/anthropics/anthropic-sdk-go"
	"github.com/anthropics/anthropic-sdk-go/option"
	"github.com/google/uuid"
	"github.com/honeycombio/beeline-go"
	"github.com/pebble-dev/bobby-assistant/service/assistant/config"
	"github.com/pebble-dev/bobby-assistant/service/assistant/functions"
	"github.com/pebble-dev/bobby-assistant/service/assistant/llm"
	"github.com/pebble-dev/bobby-assistant/service/assistant/persistence"
	"github.com/pebble-dev/bobby-assistant/service/assistant/query"
	"github.com/pebble-dev/bobby-assistant/service/assistant/quota"
	"github.com/pebble-dev/bobby-assistant/service/assistant/verifier"
	"github.com/pebble-dev/bobby-assistant/service/assistant/widgets"
	"github.com/redis/go-redis/v9"
	"nhooyr.io/websocket"
)

type PromptSession struct {
	conn             *websocket.Conn
	prompt           string
	userToken        string
	query            url.Values
	redis            *redis.Client
	threadId         uuid.UUID
	originalThreadId string
	userId           int
}

type QueryContext struct {
	values url.Values
}

func NewPromptSession(redisClient *redis.Client, rw http.ResponseWriter, r *http.Request) (*PromptSession, error) {
	prompt := r.URL.Query().Get("prompt")
	userToken := r.URL.Query().Get("token")
	originalThreadId := r.URL.Query().Get("threadId")
	c, err := websocket.Accept(rw, r, &websocket.AcceptOptions{
		OriginPatterns:     []string{"null"},
		InsecureSkipVerify: true,
	})
	if err != nil {
		return nil, err
	}

	return &PromptSession{
		conn:             c,
		prompt:           prompt,
		userToken:        userToken,
		query:            r.URL.Query(),
		redis:            redisClient,
		threadId:         uuid.New(),
		originalThreadId: originalThreadId,
	}, nil
}

func (ps *PromptSession) Run(ctx context.Context) {
	ctx = query.ContextWith(ctx, ps.query)
	client := anthropic.NewClient(option.WithAPIKey(config.GetConfig().AnthropicKey))

	var messages []anthropic.MessageParam
	messages = append(messages, anthropic.NewUserMessage(anthropic.NewTextBlock(ps.prompt)))

	if ps.originalThreadId != "" {
		var threadContext *persistence.ThreadContext
		var err error
		ctx, threadContext, err = ps.restoreContext(ctx, ps.originalThreadId)
		if err != nil {
			log.Printf("error restoring thread: %v\n", err)
			_ = ps.conn.Close(websocket.StatusInternalError, "Error restoring thread.")
			return
		}
		oldMessages := ps.restoreThread(threadContext)
		messages = append(oldMessages, messages...)
	}
	query.ThreadContextFromContext(ctx).ThreadId = ps.threadId
	user, err := quota.GetUserInfo(ctx, ps.userToken)
	if err != nil {
		log.Printf("get user info failed: %v\n", err)
		_ = ps.conn.Close(websocket.StatusInternalError, "get user info failed")
		return
	}
	ps.userId = user.UserId
	beeline.AddField(ctx, "user_id", user.UserId)
	if !user.HasSubscription && !config.GetConfig().SelfHosted {
		beeline.AddField(ctx, "error", "no subscription")
		log.Printf("user %d has no subscription\n", user.UserId)
		_ = ps.conn.Close(websocket.StatusPolicyViolation, "You need an active Rebble subscription to use Bobby.")
		return
	}
	qt := quota.NewTracker(ps.redis, user.UserId)
	used, remaining, err := qt.GetQuota(ctx)
	if err != nil {
		log.Printf("get quota failed: %v\n", err)
		_ = ps.conn.Close(websocket.StatusInternalError, "Quota lookup failed.")
		return
	}
	if remaining < 1 {
		log.Printf("quota exceeded for user %d\n", user.UserId)
		_ = ps.conn.Close(websocket.StatusPolicyViolation, "You have exceeded your quota for this month.")
		return
	}
	log.Printf("user %d has used %d / %d credits\n", user.UserId, used, remaining)
	totalInputTokens := 0
	totalCachedInputTokens := 0
	totalOutputTokens := 0
	iterations := 0
	for {
		cont, err := func() (bool, error) {
			ctx, span := beeline.StartSpan(ctx, "chat_iteration")
			defer span.Send()
			iterations++
			tools := llm.ToolsFromGenAI(functions.GetFunctionDefinitionsForCapabilities(query.SupportedActionsFromContext(ctx)))
			systemPrompt := ps.generateSystemPrompt(ctx)
			streamCtx, streamSpan := beeline.StartSpan(ctx, "chat_stream")
			params := anthropic.MessageNewParams{
				Model:       anthropic.Model(config.GetConfig().ChatModel),
				MaxTokens:   4096,
				Temperature: anthropic.Float(0.5),
				System:      []anthropic.TextBlockParam{{Text: systemPrompt}},
				Messages:    messages,
				Tools:       tools,
			}
			if iterations > 10 {
				// Force a final answer. The tool definitions must stay in the
				// request (history contains tool_use blocks), but the model may
				// not call them any more.
				params.ToolChoice = anthropic.ToolChoiceUnionParam{OfNone: &anthropic.ToolChoiceNoneParam{}}
			}
			stream := client.Messages.NewStreaming(streamCtx, params)
			message := anthropic.Message{}
			content := ""
			bufferedContent := ""
			leftTrimming := false
		read_loop:
			for stream.Next() {
				event := stream.Current()
				if err := message.Accumulate(event); err != nil {
					log.Printf("accumulate event failed: %v\n", err)
				}
				deltaEvent, ok := event.AsAny().(anthropic.ContentBlockDeltaEvent)
				if !ok {
					continue
				}
				textDelta, ok := deltaEvent.Delta.AsAny().(anthropic.TextDelta)
				if !ok {
					continue
				}
				ourContent := fixUnsupportedCharacters(textDelta.Text)
				if bufferedContent != "" {
					bufferedContent += ourContent
					closers := strings.Count(bufferedContent, "!>") + strings.Count(bufferedContent, "/>")
					if strings.Count(bufferedContent, "<!") != closers || strings.HasSuffix(bufferedContent, "<") {
						continue
					} else {
						ourContent = bufferedContent
						bufferedContent = ""
					}
				} else {
					closers := strings.Count(ourContent, "!>") + strings.Count(ourContent, "/>")
					// Streaming deltas can split a widget marker anywhere, so
					// also buffer when the delta ends on a potential "<!" start.
					if strings.Count(ourContent, "<!") != closers || strings.HasSuffix(ourContent, "<") {
						bufferedContent += ourContent
						continue
					}
				}
				if strings.TrimSpace(ourContent) != "" {
					streamContent := ourContent
					re := regexp.MustCompile(`(?s)\s*<!.+?[!/]>\s*`)
					widget := re.FindAllString(ourContent, -1)
					splitting := true
					if len(widget) > 0 {
						for _, w := range widget {
							processed, err := widgets.ProcessWidget(ctx, qt, w)
							replacement := ""
							if err != nil {
								log.Printf("process widget failed: %v\n", err)
								replacement = "(widget processing failed)"
							} else {
								jsoned, err := json.Marshal(processed)
								if err != nil {
									log.Printf("marshal widget failed: %v\n", err)
									replacement = "(widget processing failed)"
								} else {
									splitting = false
									replacement = "<<!!WIDGET:" + string(jsoned) + "!!>>"
								}
							}
							streamContent = strings.Replace(streamContent, w, replacement, 1)
							if strings.HasSuffix(streamContent, "!!>>") {
								leftTrimming = true
							}
						}
					}
					// If the last thing we generated was a widget, it's possible the model will try to put some
					// newlines or spaces in front of the next text. We don't want that, so strip it out.
					if leftTrimming {
						streamContent = strings.TrimLeft(streamContent, " \r\n\t")
					}
					if strings.TrimSpace(streamContent) != "" {
						var words []string
						if splitting {
							words = strings.Split(streamContent, " ")
							leftTrimming = false
						} else {
							words = []string{streamContent}
						}
						for i, w := range words {
							if i != len(words)-1 {
								w += " "
							}
							if err := ps.conn.Write(streamCtx, websocket.MessageText, []byte("c"+w)); err != nil {
								streamSpan.AddField("error", err)
								log.Printf("write to websocket failed: %v\n", err)
								break read_loop
							}
							time.Sleep(time.Millisecond * 40)
						}
					}
				}
				content += ourContent
			}
			if err := stream.Err(); err != nil {
				streamSpan.AddField("error", err)
				log.Printf("recv from Anthropic failed: %v\n", err)
				// This comes up when the API is over capacity, which does happen sometimes.
				// There's nothing we can really do here, though we could blame them instead of ourselves.
				_ = ps.conn.Close(websocket.StatusInternalError, "Bobby is unavailable right now. Please try again in a few moments.")
				streamSpan.Send()
				return false, err
			}
			streamSpan.Send()
			usage := message.Usage
			if usage.InputTokens != 0 || usage.CacheReadInputTokens != 0 {
				// Anthropic reports cached tokens separately from input_tokens;
				// the quota tracker expects the cached count to be included.
				promptTokens := int(usage.InputTokens + usage.CacheReadInputTokens)
				if _, err := qt.ChargeInputQuota(ctx, promptTokens, int(usage.CacheReadInputTokens)); err != nil {
					log.Printf("charge input quota failed: %v\n", err)
				}
				totalInputTokens += promptTokens
				totalCachedInputTokens += int(usage.CacheReadInputTokens)
			}
			if usage.OutputTokens != 0 {
				if _, err := qt.ChargeOutputQuota(ctx, int(usage.OutputTokens)); err != nil {
					log.Printf("charge output quota failed: %v\n", err)
				}
				totalOutputTokens += int(usage.OutputTokens)
			}
			messages = append(messages, message.ToParam())
			var toolUses []anthropic.ToolUseBlock
			for _, block := range message.Content {
				if tu, ok := block.AsAny().(anthropic.ToolUseBlock); ok {
					toolUses = append(toolUses, tu)
				}
			}
			if message.StopReason == anthropic.StopReasonToolUse && len(toolUses) > 0 {
				var results []anthropic.ContentBlockParamUnion
				for _, tu := range toolUses {
					log.Printf("calling function %s\n", tu.Name)
					fnArgs := string(tu.Input)
					if err := ps.conn.Write(ctx, websocket.MessageText, []byte("f"+functions.SummariseFunction(tu.Name, fnArgs))); err != nil {
						log.Printf("write to websocket failed: %v\n", err)
						return false, err
					}
					var result string
					var err error
					if functions.IsAction(tu.Name) {
						result, err = functions.CallAction(ctx, qt, tu.Name, fnArgs, ps.conn)
					} else {
						result, err = functions.CallFunction(ctx, qt, tu.Name, fnArgs)
					}
					isError := false
					if err != nil {
						log.Printf("call function failed: %v\n", err)
						result = "failed to call function: " + err.Error()
						isError = true
					}
					results = append(results, anthropic.NewToolResultBlock(tu.ID, result, isError))
				}
				messages = append(messages, anthropic.NewUserMessage(results...))
				return true, nil
			}
			return false, nil
		}()
		if err != nil {
			return
		}
		if !cont {
			log.Println("Stopping")
			break
		}
		log.Println("Going around again")
	}

	lies, err := verifier.FindLies(ctx, qt, messages)
	if err != nil {
		// Bobby doesn't usually lie, so this isn't worth killing the session over.
		log.Printf("find lies failed: %v\n", err)
	}
	if len(lies) > 0 {
		beeline.AddField(ctx, "lies", lies)
		log.Printf("lies detected: %v\n", lies)
		var formattedLies []string
		for _, l := range lies {
			switch l {
			case "alarm":
				formattedLies = append(formattedLies, "set an alarm")
			case "timer":
				formattedLies = append(formattedLies, "set a timer")
			case "reminder":
				formattedLies = append(formattedLies, "set a reminder")
			case "settings":
				formattedLies = append(formattedLies, "change any settings")
			}
		}
		prettyLies := strings.Join(formattedLies, ", ")
		if len(formattedLies) > 1 {
			prettyLies = strings.Join(formattedLies[:len(formattedLies)-1], ", ") + ", or " + formattedLies[len(formattedLies)-1]
		}
		message := "Bobby did not, in fact, " + prettyLies + "."
		if err := ps.conn.Write(ctx, websocket.MessageText, []byte("w"+message)); err != nil {
			log.Printf("write to websocket failed: %v\n", err)
		}
	}

	if err := ps.conn.Write(ctx, websocket.MessageText, []byte("d")); err != nil {
		log.Printf("write to websocket failed: %v\n", err)
	}

	beeline.AddField(ctx, "total_input_tokens", totalInputTokens)
	beeline.AddField(ctx, "total_output_tokens", totalOutputTokens)
	beeline.AddField(ctx, "total_cached_output_tokens", totalCachedInputTokens)
	beeline.AddField(ctx, "total_cost", (totalInputTokens-totalCachedInputTokens)*quota.InputTokenCredits+totalCachedInputTokens*quota.CachedInputTokenCredits+totalOutputTokens*quota.OutputTokenCredits)
	if err := ps.storeThread(ctx, messages); err != nil {
		log.Printf("store thread failed: %v\n", err)
		_ = ps.conn.Close(websocket.StatusInternalError, "store thread failed")
		return
	}
	if err := ps.conn.Write(ctx, websocket.MessageText, []byte("t"+ps.threadId.String())); err != nil {
		log.Printf("store thread ID failed: %s\n", err)
	}
	log.Println("Request handled successfully.")
	_ = ps.conn.Close(websocket.StatusNormalClosure, "")
}

func fixUnsupportedCharacters(s string) string {
	// Replace the narrow non-breaking space with a regular non-breaking space.
	return strings.ReplaceAll(s, "\u202f", "\u00a0")
}

func (ps *PromptSession) storeThread(ctx context.Context, messages []anthropic.MessageParam) error {
	ctx, span := beeline.StartSpan(ctx, "store_thread")
	defer span.Send()
	var toStore []persistence.SerializedMessage
	toolNames := map[string]string{}
	for _, m := range messages {
		role := "user"
		if m.Role == anthropic.MessageParamRoleAssistant {
			role = "model"
		}
		for _, block := range m.Content {
			switch {
			case block.OfText != nil:
				if len(strings.TrimSpace(block.OfText.Text)) > 0 {
					toStore = append(toStore, persistence.SerializedMessage{
						Role:    role,
						Content: block.OfText.Text,
					})
				}
			case block.OfToolUse != nil:
				tu := block.OfToolUse
				toolNames[tu.ID] = tu.Name
				toStore = append(toStore, persistence.SerializedMessage{
					Role: role,
					FunctionCall: &persistence.FunctionCall{
						ID:   tu.ID,
						Name: tu.Name,
						Args: anyToMap(tu.Input),
					},
				})
			case block.OfToolResult != nil:
				tr := block.OfToolResult
				name := toolNames[tr.ToolUseID]
				text := ""
				for _, c := range tr.Content {
					if c.OfText != nil {
						text += c.OfText.Text
					}
				}
				var response map[string]any
				if err := json.Unmarshal([]byte(text), &response); err != nil || response == nil {
					response = map[string]any{"result": text}
				}
				fnInfo := functions.GetFunctionRegistration(name)
				if fnInfo != nil && fnInfo.RedactOutputInChatHistory {
					response = map[string]any{"redacted": "redacted to reduce context size, call again if necessary"}
				}
				toStore = append(toStore, persistence.SerializedMessage{
					Role: "function",
					FunctionResponse: &persistence.FunctionResponse{
						ID:       tr.ToolUseID,
						Name:     name,
						Response: response,
					},
				})
			}
		}
	}
	threadContext := query.ThreadContextFromContext(ctx)
	threadContext.Messages = toStore
	return persistence.StoreThread(ctx, ps.redis, threadContext)
}

// anyToMap converts a tool input (json.RawMessage from a model response, or a
// map restored from storage) into a plain map for serialization.
func anyToMap(input any) map[string]any {
	raw, err := json.Marshal(input)
	if err != nil {
		return nil
	}
	var m map[string]any
	if err := json.Unmarshal(raw, &m); err != nil {
		return nil
	}
	return m
}

func (ps *PromptSession) restoreContext(ctx context.Context, oldThreadId string) (context.Context, *persistence.ThreadContext, error) {
	threadContext, err := persistence.LoadThread(ctx, ps.redis, oldThreadId)
	if err != nil {
		return ctx, nil, err
	}
	ctx = query.ContextWithThread(ctx, threadContext)

	return ctx, threadContext, nil
}

func (ps *PromptSession) restoreThread(threadContext *persistence.ThreadContext) []anthropic.MessageParam {
	var result []anthropic.MessageParam
	syntheticIds := 0
	lastToolUseId := ""
	for _, m := range threadContext.Messages {
		switch {
		case m.FunctionCall != nil:
			id := m.FunctionCall.ID
			if id == "" {
				// Threads stored before the Anthropic migration have no tool
				// call IDs; synthesize matching pairs.
				syntheticIds++
				id = fmt.Sprintf("toolu_restored_%d", syntheticIds)
			}
			lastToolUseId = id
			result = append(result, anthropic.NewAssistantMessage(
				anthropic.NewToolUseBlock(id, m.FunctionCall.Args, m.FunctionCall.Name)))
		case m.FunctionResponse != nil:
			id := m.FunctionResponse.ID
			if id == "" {
				id = lastToolUseId
			}
			if id == "" {
				continue
			}
			respBytes, err := json.Marshal(m.FunctionResponse.Response)
			if err != nil {
				continue
			}
			result = append(result, anthropic.NewUserMessage(
				anthropic.NewToolResultBlock(id, string(respBytes), false)))
		case m.Role == "user":
			result = append(result, anthropic.NewUserMessage(anthropic.NewTextBlock(m.Content)))
		case strings.TrimSpace(m.Content) != "":
			result = append(result, anthropic.NewAssistantMessage(anthropic.NewTextBlock(m.Content)))
		}
	}
	return result
}
