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

package config

import (
	"log"
	"os"
	"strings"

	"github.com/joho/godotenv"
)

// TODO: something reasonable.

type Config struct {
	BaseURL                string
	// SelfHosted disables the Rebble subscription requirement and the monthly
	// quota cap for personal deployments. Set SELF_HOSTED=1 (or true/yes).
	SelfHosted             bool
	GeminiKey              string
	AnthropicKey           string
	// ChatModel and VerifierModel are exact Anthropic model ID strings, set
	// via CHAT_MODEL / VERIFIER_MODEL. Changing tier is a deploy-time
	// setting, not a code change.
	ChatModel              string
	VerifierModel          string
	// PerplexityKey enables the web_search function. If unset, the function
	// is not registered and the model never sees it.
	PerplexityKey          string
	MapboxKey              string
	IBMKey                 string
	ExchangeRateApiKey     string
	RedisURL               string
	UserIdentificationURL  string
	HoneycombKey           string
	DiscordFeedbackURL     string
	GoogleMapsStaticKey    string
	GoogleMapsStaticSecret string
	GoogleMapsStaticMapId  string
}

var c Config

func GetConfig() *Config {
	return &c
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func isTruthy(s string) bool {
	switch strings.ToLower(s) {
	case "1", "true", "yes", "on":
		return true
	}
	return false
}

func init() {
	// Load .env file if it exists
	if err := godotenv.Load(); err != nil {
		// Only log if the file exists but couldn't be loaded
		if !os.IsNotExist(err) {
			log.Printf("Error loading .env file: %v", err)
		}
	}

	c = Config{
		BaseURL:                os.Getenv("BASE_URL"),
		SelfHosted:             isTruthy(os.Getenv("SELF_HOSTED")),
		GeminiKey:              os.Getenv("GEMINI_KEY"),
		AnthropicKey:           os.Getenv("ANTHROPIC_API_KEY"),
		ChatModel:              envOr("CHAT_MODEL", "claude-haiku-4-5"),
		VerifierModel:          envOr("VERIFIER_MODEL", "claude-haiku-4-5"),
		PerplexityKey:          os.Getenv("PERPLEXITY_API_KEY"),
		MapboxKey:              os.Getenv("MAPBOX_KEY"),
		IBMKey:                 os.Getenv("IBM_KEY"),
		ExchangeRateApiKey:     os.Getenv("EXCHANGE_RATE_API_KEY"),
		RedisURL:               os.Getenv("REDIS_URL"),
		UserIdentificationURL:  os.Getenv("USER_IDENTIFICATION_URL"),
		HoneycombKey:           os.Getenv("HONEYCOMB_KEY"),
		DiscordFeedbackURL:     os.Getenv("DISCORD_FEEDBACK_URL"),
		GoogleMapsStaticKey:    os.Getenv("GOOGLE_MAPS_STATIC_KEY"),
		GoogleMapsStaticSecret: os.Getenv("GOOGLE_MAPS_STATIC_SECRET"),
		GoogleMapsStaticMapId:  os.Getenv("GOOGLE_MAPS_STATIC_MAP_ID"),
	}
}
