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

// Package llm bridges the genai-typed function registry to the Anthropic
// Messages API. The registry (functions/*) stays genai-typed so the function
// implementations need no changes when switching providers.
package llm

import (
	"github.com/anthropics/anthropic-sdk-go"
	"google.golang.org/genai"
)

// ToolsFromGenAI converts the registry's genai.FunctionDeclaration list into
// Anthropic tool definitions.
func ToolsFromGenAI(decls []*genai.FunctionDeclaration) []anthropic.ToolUnionParam {
	tools := make([]anthropic.ToolUnionParam, 0, len(decls))
	for _, d := range decls {
		tool := anthropic.ToolParam{
			Name:        d.Name,
			Description: anthropic.String(d.Description),
			InputSchema: inputSchemaFromGenAI(d.Parameters),
		}
		tools = append(tools, anthropic.ToolUnionParam{OfTool: &tool})
	}
	return tools
}

func inputSchemaFromGenAI(s *genai.Schema) anthropic.ToolInputSchemaParam {
	out := anthropic.ToolInputSchemaParam{}
	if s == nil {
		return out
	}
	if len(s.Properties) > 0 {
		props := make(map[string]any, len(s.Properties))
		for name, prop := range s.Properties {
			props[name] = schemaToJSONSchema(prop)
		}
		out.Properties = props
	}
	out.Required = s.Required
	return out
}

// schemaToJSONSchema converts a genai.Schema to a plain JSON-schema map.
// Gemini-specific numeric format hints ("int32", "double") are dropped; the
// JSON-schema type already conveys them.
func schemaToJSONSchema(s *genai.Schema) map[string]any {
	if s == nil {
		return nil
	}
	m := map[string]any{}
	if t := jsonSchemaType(s.Type); t != "" {
		if s.Nullable != nil && *s.Nullable {
			m["type"] = []string{t, "null"}
		} else {
			m["type"] = t
		}
	}
	if s.Description != "" {
		m["description"] = s.Description
	}
	if len(s.Enum) > 0 {
		m["enum"] = s.Enum
	}
	if s.Items != nil {
		m["items"] = schemaToJSONSchema(s.Items)
	}
	if len(s.Properties) > 0 {
		props := make(map[string]any, len(s.Properties))
		for name, prop := range s.Properties {
			props[name] = schemaToJSONSchema(prop)
		}
		m["properties"] = props
	}
	if len(s.Required) > 0 {
		m["required"] = s.Required
	}
	return m
}

func jsonSchemaType(t genai.Type) string {
	switch t {
	case genai.TypeString:
		return "string"
	case genai.TypeNumber:
		return "number"
	case genai.TypeInteger:
		return "integer"
	case genai.TypeBoolean:
		return "boolean"
	case genai.TypeArray:
		return "array"
	case genai.TypeObject:
		return "object"
	}
	return ""
}
