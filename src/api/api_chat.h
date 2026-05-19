#pragma once
/*
 * api_chat.h
 * ─────────────────────────────────────────────────────────────────
 * Chat Q&A about the meeting using OpenAI with the final summary
 * and transcript as grounding context.
 * ─────────────────────────────────────────────────────────────────
 */

#include "../core/globals.h"

// Ask a natural-language question about the meeting.
// historyJson: JSON array of prior {role,content} messages (last ~8 turns).
//              Pass "" to start fresh.
// overrideContext: if non-empty, use this summary text instead of finalSummaryText.
//                  Used when asking about a historical (past) meeting from the History tab.
// Returns the model's answer as a plain String.
String askAboutSummary(const String& question, const String& historyJson, const String& overrideContext = "");