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
// historyJson:     JSON array of prior {role,content} messages (last ~8 turns).
//                  Pass "" to start fresh.
// overrideContext: if non-empty, use this summary text instead of finalSummaryText.
// meetingDir:      if non-empty (e.g. "/meeting_2026-05-28_17-58-33"), the chat
//                  reads the FULL transcript from full_transcript.txt in that
//                  directory and uses it as grounding instead of the small RAM
//                  tail.  Pass "" for the current/just-finished meeting (uses
//                  fullTranscript in RAM).
// Returns the model's answer as a plain String.
String askAboutSummary(const String& question,
                       const String& historyJson,
                       const String& overrideContext = "",
                       const String& meetingDir      = "");