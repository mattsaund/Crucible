// SPDX-License-Identifier: MIT
//! What the engine says is going on, as it is pushed.

use crate::text::clean;
use serde_json::Value;
use std::collections::HashMap;
use std::rc::Rc;

pub fn s(v: &Value, key: &str) -> String {
    v.get(key).and_then(Value::as_str).unwrap_or("").to_string()
}
pub fn n(v: &Value, key: &str) -> f64 {
    v.get(key).and_then(Value::as_f64).unwrap_or(0.0)
}
pub fn b(v: &Value, key: &str) -> bool {
    v.get(key).and_then(Value::as_bool).unwrap_or(false)
}
/// A value by a dotted path.
pub fn at<'a>(v: &'a Value, path: &str) -> Option<&'a Value> {
    path.split('.').try_fold(v, |node, key| node.get(key))
}

#[derive(Clone, Default)]
pub struct Action {
    pub summary: String,
    pub body: String,
    pub language: String,
    pub picture: String,
}

#[derive(Clone, Default)]
pub struct Route {
    pub expert: String,
    pub confidence: f64,
    pub source: String,
}

#[derive(Clone, Default)]
pub struct Turn {
    pub rev: u64,
    pub prompt: String,
    pub reply: String,
    pub reasoning: String,
    pub streaming: bool,
    pub canceled: bool,
    pub failed: bool,
    pub tps: f64,
    pub prompt_tokens: f64,
    pub output_tokens: f64,
    pub load_ms: f64,
    pub route: Option<Route>,
    pub actions: Vec<Action>,
    pub attachments: Vec<String>,
    /// The thinking, in the order it happened: what came before each action,
    /// then what came after the last.
    pub thoughts: Vec<String>,
}

/// The reasoning, cut where each action happened. A turn saved before the
/// engine kept that has it all in front of the first action.
fn split_thoughts(raw: &str, marks: &[Option<usize>]) -> Vec<String> {
    if marks.iter().all(Option::is_none) {
        let mut out = vec![clean(raw)];
        out.resize(marks.len() + 1, String::new());
        return out;
    }
    let mut out = Vec::with_capacity(marks.len() + 1);
    let mut from = 0;
    for mark in marks {
        let mut to = mark.unwrap_or(from).clamp(from, raw.len());
        while !raw.is_char_boundary(to) {
            to += 1;
        }
        out.push(clean(&raw[from..to]));
        from = to;
    }
    out.push(clean(&raw[from..]));
    out
}

impl Turn {
    fn parse(v: &Value) -> Turn {
        let list = v.get("actions").and_then(Value::as_array);
        let marks: Vec<Option<usize>> = list
            .map(|l| l.iter().map(|a| a.get("thought_at").and_then(Value::as_u64).map(|m| m as usize)).collect())
            .unwrap_or_default();
        Turn {
            thoughts: split_thoughts(&s(v, "reasoning"), &marks),
            rev: v.get("rev").and_then(Value::as_u64).unwrap_or(0),
            prompt: clean(&s(v, "prompt")),
            reply: clean(&s(v, "reply")),
            reasoning: clean(&s(v, "reasoning")),
            streaming: b(v, "streaming"),
            canceled: b(v, "canceled"),
            failed: b(v, "failed"),
            tps: n(v, "tokens_per_second"),
            prompt_tokens: n(v, "prompt_tokens"),
            output_tokens: n(v, "output_tokens"),
            load_ms: n(v, "load_ms"),
            route: v.get("route").map(|r| Route {
                expert: s(r, "expert"),
                confidence: n(r, "confidence"),
                source: s(r, "source"),
            }),
            actions: v
                .get("actions")
                .and_then(Value::as_array)
                .map(|list| {
                    list.iter()
                        .map(|a| Action {
                            summary: clean(&s(a, "summary")),
                            body: clean(&s(a, "body")),
                            language: s(a, "language"),
                            picture: s(a, "picture"),
                        })
                        .collect()
                })
                .unwrap_or_default(),
            attachments: v
                .get("attachments")
                .and_then(Value::as_array)
                .map(|list| list.iter().map(|a| s(a, "name")).collect())
                .unwrap_or_default(),
        }
    }
}

#[derive(Clone, Default)]
pub struct Expert {
    pub id: String,
    pub name: String,
    pub tag: String,
    pub blurb: String,
    pub phase: String,
    pub progress: f64,
    pub provider: String,
    pub model: String,
    pub default: bool,
}

#[derive(Clone, Default, PartialEq)]
pub struct PendingEdit {
    pub path: String,
    pub before: String,
    pub after: String,
}

/// The newest push, with its turns put back whole.
#[derive(Default)]
pub struct State {
    pub raw: Value,
    pub turns: Vec<Rc<Turn>>,
    pub experts: Vec<Expert>,
    pub pending_edit: Option<PendingEdit>,
    pub bench: Option<Value>,
    pub busy: bool,
    pub mood: String,
    pub status: String,
    pub revision: u64,
}

impl State {
    pub fn apply(&mut self, mut v: Value) {
        let same: Vec<String> = v
            .get("same")
            .and_then(Value::as_array)
            .map(|l| l.iter().filter_map(|x| x.as_str().map(str::to_string)).collect())
            .unwrap_or_default();
        if !same.iter().any(|x| x == "pending_edit") {
            self.pending_edit = v.get("pending_edit").map(|e| PendingEdit {
                path: clean(&s(e, "path")),
                before: clean(&s(e, "before")),
                after: clean(&s(e, "after")),
            });
        }
        if !same.iter().any(|x| x == "bench") {
            self.bench = v.get("bench").cloned();
        }
        if let Some(Value::Array(list)) = v.get_mut("turns").map(Value::take) {
            let known: HashMap<u64, Rc<Turn>> =
                self.turns.iter().filter(|t| t.rev != 0).map(|t| (t.rev, Rc::clone(t))).collect();
            self.turns = list
                .iter()
                .filter_map(|one| {
                    if b(one, "stub") {
                        known.get(&(n(one, "rev") as u64)).cloned()
                    } else {
                        Some(Rc::new(Turn::parse(one)))
                    }
                })
                .collect();
        }
        self.experts = v
            .get("experts")
            .and_then(Value::as_array)
            .map(|list| {
                list.iter()
                    .map(|e| Expert {
                        id: s(e, "id"),
                        name: s(e, "name"),
                        tag: s(e, "tag"),
                        blurb: s(e, "blurb"),
                        phase: s(e, "phase"),
                        progress: n(e, "progress"),
                        provider: s(e, "provider"),
                        model: s(e, "model"),
                        default: b(e, "default"),
                    })
                    .collect()
            })
            .unwrap_or_default();
        self.busy = b(&v, "busy");
        self.mood = s(&v, "mood");
        self.status = clean(&s(&v, "status"));
        self.raw = v;
        self.revision += 1;
    }

    pub fn expert_name(&self, id: &str) -> String {
        self.experts.iter().find(|e| e.id == id).map_or_else(|| id.to_string(), |e| e.name.clone())
    }

    /// Who has the prompt right now, or who last did.
    pub fn expert_now(&self) -> Option<&Expert> {
        let id = self
            .raw
            .get("linked")
            .and_then(Value::as_str)
            .or_else(|| self.raw.get("resident").and_then(Value::as_str))?;
        self.experts.iter().find(|e| e.id == id)
    }

    pub fn project_display(&self) -> String {
        let Some(project) = self.raw.get("project") else { return String::new() };
        if b(project, "open") {
            let shown = s(project, "display");
            if shown.is_empty() { s(project, "root") } else { shown }
        } else {
            s(project, "scratch_display")
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn thinking_is_cut_where_each_action_happened() {
        let turn = Turn::parse(&json!({
            "reasoning": "first.\n\nsecond.\n\nthird.",
            "actions": [{"summary": "read a", "thought_at": 6}, {"summary": "ran b", "thought_at": 15}],
        }));
        let parts: Vec<&str> = turn.thoughts.iter().map(|t| t.trim()).collect();
        assert_eq!(parts, vec!["first.", "second.", "third."]);
    }

    #[test]
    fn nothing_before_the_first_action_stays_nothing() {
        let turn = Turn::parse(&json!({"reasoning": "after it", "actions": [{"summary": "read a", "thought_at": 0}]}));
        assert_eq!(turn.thoughts, vec!["".to_string(), "after it".to_string()]);
    }

    #[test]
    fn a_turn_saved_before_the_marks_has_its_thinking_first() {
        let turn = Turn::parse(&json!({"reasoning": "all of it", "actions": [{"summary": "read a"}, {"summary": "b"}]}));
        assert_eq!(turn.thoughts, vec!["all of it".to_string(), String::new(), String::new()]);
    }

    #[test]
    fn a_mark_inside_a_character_does_not_panic() {
        let turn = Turn::parse(&json!({"reasoning": "héllo", "actions": [{"summary": "a", "thought_at": 2}]}));
        assert_eq!(turn.thoughts.concat(), "héllo");
    }
}
