//! Conversation Rubric Tags (`tags.rs`) for BeatrixLeslie IDE (`spell_core`).
//!
//! Implements Decision 4: Replaces coding-only `skills` with general-purpose `tags`
//! (`ConversationTag` & `TagRegistry`) applicable to any conversation rubric across
//! research, writing, analysis, engineering, governance, and general workflows.
//! Note: BeatrixLeslie IDE intentionally excludes any `spawn_agent` tool.

use std::collections::HashMap;
use std::fs;
use std::path::Path;
use std::sync::Arc;

use parking_lot::RwLock;
use serde::{Deserialize, Serialize};

/// A general-purpose conversation rubric tag that guides reasoning, structure,
/// evaluation criteria, tool scoping, and sampling parameters for any domain.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct ConversationTag {
    pub id: String,
    pub name: String,
    /// Domain category, e.g., `"research"`, `"writing"`, `"analysis"`, `"engineering"`, `"governance"`, `"general"`.
    #[serde(default)]
    pub category: String,
    #[serde(default)]
    pub description: String,
    #[serde(default)]
    pub rubric_prompt: String,
    #[serde(default)]
    pub is_builtin: bool,
    /// Optional inline trigger token (e.g. `"@tag:research-synthesis"` or `"@skill:refactor"`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub trigger: Option<String>,
    /// Optional per-turn tool whitelist when this tag/skill is triggered.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub allowed_tools: Option<Vec<String>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub temperature: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub top_p: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub presence_penalty: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub repetition_penalty: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_tokens: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enable_thinking: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thinking_budget: Option<u64>,
}

impl ConversationTag {
    /// Creates a new [`ConversationTag`].
    pub fn new(
        id: impl Into<String>,
        name: impl Into<String>,
        category: impl Into<String>,
        description: impl Into<String>,
        rubric_prompt: impl Into<String>,
        is_builtin: bool,
    ) -> Self {
        Self {
            id: id.into(),
            name: name.into(),
            category: category.into(),
            description: description.into(),
            rubric_prompt: rubric_prompt.into(),
            is_builtin,
            ..Default::default()
        }
    }
}

/// A per-turn inline steering directive (`#steer:<slug>`) that adjusts thinking mode,
/// sampling parameters, and tail-injected system instructions without mutating `messages[0]`.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct SteeringDirective {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub trigger: String,
    #[serde(default)]
    pub description: String,
    #[serde(default)]
    pub system_append: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub temperature: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub top_p: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub presence_penalty: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub repetition_penalty: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_tokens: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enable_thinking: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thinking_budget: Option<u64>,
    #[serde(default)]
    pub is_builtin: bool,
}

/// Result of deterministic inline `@tag:`, `@skill:`, and `#steer:` prompt preprocessing.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct PreprocessedPrompt {
    pub raw_prompt: String,
    pub cleaned_prompt: String,
    pub matched_tags: Vec<ConversationTag>,
    pub matched_steering: Vec<SteeringDirective>,
    pub allowed_tools: Option<Vec<String>>,
    pub temperature: Option<f64>,
    pub top_p: Option<f64>,
    pub presence_penalty: Option<f64>,
    pub repetition_penalty: Option<f64>,
    pub max_tokens: Option<u64>,
    pub enable_thinking: Option<bool>,
    pub thinking_budget: Option<u64>,
    pub chat_template_kwargs: Option<serde_json::Value>,
    pub tail_system_append: String,
}

impl PreprocessedPrompt {
    /// Returns `cleaned_prompt` if `tail_system_append` is empty, or appends
    /// `\n\n<turn_rubric_and_steering>\n...\n</turn_rubric_and_steering>` to `cleaned_prompt`
    /// so `messages[0]` is never mutated mid-thread (`WIRE-03` & `G3`).
    pub fn build_tail_injected_user_message(&self) -> String {
        let tail = self.tail_system_append.trim();
        if tail.is_empty() {
            self.cleaned_prompt.clone()
        } else {
            format!(
                "{}\n\n<turn_rubric_and_steering>\n{}\n</turn_rubric_and_steering>",
                self.cleaned_prompt, tail
            )
        }
    }
}

/// Normalizes a tag identifier or display name into a canonical slug for lookup.
pub fn normalize_tag_key(raw: &str) -> String {
    raw.trim()
        .to_lowercase()
        .chars()
        .map(|c| if c == '_' || c == ' ' { '-' } else { c })
        .collect()
}

/// Returns the 5 built-in conversation rubric tags covering diverse non-coding and coding domains.
pub fn builtin_conversation_tags() -> Vec<ConversationTag> {
    vec![
        ConversationTag {
            id: "research-synthesis".to_string(),
            name: "research-synthesis".to_string(),
            category: "research".to_string(),
            description: "Evidence-backed synthesis, primary-source verification, explicit confidence levels, and structured trade-off matrices.".to_string(),
            rubric_prompt: concat!(
                "### Rubric: Research Synthesis\n",
                "1. **Primary-Source Grounding**: Anchor every technical or factual claim to verifiable primary sources, citations, or empirical measurements.\n",
                "2. **Explicit Confidence Levels**: Annotate major conclusions with calibrated confidence levels (`High`, `Medium`, `Low`) and explain the supporting evidence weight.\n",
                "3. **Competing Hypotheses**: Compare alternative explanations or approaches objectively before converging on a synthesis.\n",
                "4. **Structured Trade-off Matrix**: Present key dimensions (latency, throughput, fidelity, cost, operational complexity, or methodological validity) in a comparative matrix."
            ).to_string(),
            is_builtin: true,
            ..Default::default()
        },
        ConversationTag {
            id: "executive-decision-memo".to_string(),
            name: "executive-decision-memo".to_string(),
            category: "writing".to_string(),
            description: "Concise BLUF (Bottom Line Up Front) framing, risk/cost/impact tables, and actionable recommendations.".to_string(),
            rubric_prompt: concat!(
                "### Rubric: Executive Decision Memo\n",
                "1. **BLUF (Bottom Line Up Front)**: Open with a crisp 2-3 sentence summary stating the core decision, recommended option, and expected outcome.\n",
                "2. **Options & Risk/Cost/Impact Table**: Evaluate each viable option across implementation cost, timeline, primary risks, and measurable impact.\n",
                "3. **Key Assumptions & Reversibility**: Identify whether the decision is a Type-1 (irreversible) or Type-2 (reversible) commitment and state critical dependencies.\n",
                "4. **Actionable Next Steps**: Conclude with concrete, owner-ready action items and verification milestones."
            ).to_string(),
            is_builtin: true,
            ..Default::default()
        },
        ConversationTag {
            id: "critical-rubric-evaluator".to_string(),
            name: "critical-rubric-evaluator".to_string(),
            category: "analysis".to_string(),
            description: "Evaluates proposals or drafts against explicit criteria, highlighting blind spots, assumptions, and counter-arguments.".to_string(),
            rubric_prompt: concat!(
                "### Rubric: Critical Rubric Evaluator\n",
                "1. **Explicit Evaluation Criteria**: Grade the subject across clarity, soundness, completeness, edge-case resilience, and empirical testability.\n",
                "2. **Hidden Assumptions & Blind Spots**: Surface unstated premises, optimistic failure-mode assumptions, or omitted stakeholder constraints.\n",
                "3. **Steelman Counter-Arguments**: Construct the strongest possible critique of the proposal and specify what evidence would falsify or validate it.\n",
                "4. **Prioritized Remediation Plan**: Rank concrete improvements by severity (`Critical`, `Major`, `Minor`) with specific corrective guidance."
            ).to_string(),
            is_builtin: true,
            ..Default::default()
        },
        ConversationTag {
            id: "systems-architecture".to_string(),
            name: "systems-architecture".to_string(),
            category: "engineering".to_string(),
            description: "Focuses on data-plane contracts, memory/cache locality, failure modes, and zero-copy boundaries.".to_string(),
            rubric_prompt: concat!(
                "### Rubric: Systems Architecture\n",
                "1. **Data-Plane & Control-Plane Contracts**: Define explicit wire formats, invariants, ownership boundaries, and backpressure semantics.\n",
                "2. **Memory & Cache Locality**: Analyze allocation lifetimes, cache-line friendliness, KV/GDN prefix stability, and zero-copy transfer paths.\n",
                "3. **Concurrency & Scheduling**: Prevent priority inversion and P-core/E-core contention; ensure bounded queues and deterministic cancellation.\n",
                "4. **Failure Modes & Observability**: Specify degraded-mode fallbacks, timeout budgets, idempotency guarantees, and structured telemetry counters."
            ).to_string(),
            is_builtin: true,
            ..Default::default()
        },
        ConversationTag {
            id: "editorial-clarity".to_string(),
            name: "editorial-clarity".to_string(),
            category: "writing".to_string(),
            description: "Plain-language precision, zero filler, active voice, and rigorous logical flow.".to_string(),
            rubric_prompt: concat!(
                "### Rubric: Editorial Clarity\n",
                "1. **Zero Filler**: Eliminate throat-clearing qualifiers, redundant adverbs, and vague abstractions; make every sentence carry concrete meaning.\n",
                "2. **Active Voice & Direct Verbs**: Prefer strong, specific verbs and clear grammatical subjects over passive constructions.\n",
                "3. **Logical Transitions**: Ensure each paragraph advances a single coherent claim that follows naturally from the preceding point.\n",
                "4. **Terminological Consistency**: Use one precise term for each concept throughout the text without ambiguous synonyms."
            ).to_string(),
            is_builtin: true,
            ..Default::default()
        },
    ]
}

/// Returns the 5 built-in `#steer:` / `#spell` directives (`deep-think`, `fast-patch`, `concise`, `deterministic`, `spell`).
pub fn builtin_steering_directives() -> Vec<SteeringDirective> {
    vec![
        SteeringDirective {
            id: "deep-think".to_string(),
            name: "deep-think".to_string(),
            trigger: "#steer:think".to_string(),
            description: "Enables extended thinking mode with calibrated reasoning sampling.".to_string(),
            system_append: String::new(),
            temperature: Some(0.6),
            top_p: Some(0.95),
            presence_penalty: Some(0.0),
            repetition_penalty: None,
            max_tokens: None,
            enable_thinking: Some(true),
            thinking_budget: None,
            is_builtin: true,
        },
        SteeringDirective {
            id: "fast-patch".to_string(),
            name: "fast-patch".to_string(),
            trigger: "#steer:fast".to_string(),
            description: "Disables extended thinking and emits direct unified diffs or replacement blocks.".to_string(),
            system_append: "Output only unified diffs or exact replacement blocks. Zero prose.".to_string(),
            temperature: Some(0.7),
            top_p: Some(0.80),
            presence_penalty: Some(1.5),
            repetition_penalty: None,
            max_tokens: None,
            enable_thinking: Some(false),
            thinking_budget: None,
            is_builtin: true,
        },
        SteeringDirective {
            id: "concise".to_string(),
            name: "concise".to_string(),
            trigger: "#steer:concise".to_string(),
            description: "Keeps responses concise, structured, and high-signal.".to_string(),
            system_append: "Keep responses concise, structured, and high-signal.".to_string(),
            temperature: Some(0.3),
            top_p: None,
            presence_penalty: None,
            repetition_penalty: None,
            max_tokens: None,
            enable_thinking: None,
            thinking_budget: None,
            is_builtin: true,
        },
        SteeringDirective {
            id: "deterministic".to_string(),
            name: "deterministic".to_string(),
            trigger: "#steer:deterministic".to_string(),
            description: "Greedy deterministic decoding (temperature = 0.0, top_p = 1.0).".to_string(),
            system_append: String::new(),
            temperature: Some(0.0),
            top_p: Some(1.0),
            presence_penalty: None,
            repetition_penalty: None,
            max_tokens: None,
            enable_thinking: None,
            thinking_budget: None,
            is_builtin: true,
        },
        SteeringDirective {
            id: "spell".to_string(),
            name: "spell".to_string(),
            trigger: "#spell".to_string(),
            description: "Closes the Message Terminal CLI and dispatches the terminal working directory and command output context to the agent.".to_string(),
            system_append: "Message Terminal #spell handoff invoked: inspect the attached CLI working directory and terminal output context and resolve the user's request directly.".to_string(),
            temperature: Some(0.4),
            top_p: Some(0.92),
            presence_penalty: None,
            repetition_penalty: None,
            max_tokens: None,
            enable_thinking: Some(true),
            thinking_budget: None,
            is_builtin: true,
        },
    ]
}

#[derive(Clone, Debug, Default)]
struct TagRegistryInner {
    /// Ordered list of tag IDs to preserve deterministic listing order.
    order: Vec<String>,
    /// Map from normalized tag ID -> `ConversationTag`.
    tags_by_id: HashMap<String, ConversationTag>,
    /// Active tag IDs applied to the current conversation context.
    active_tag_ids: Vec<String>,
    /// Ordered list of steering directive IDs to preserve deterministic listing order.
    steering_order: Vec<String>,
    /// Map from normalized steering ID -> `SteeringDirective`.
    steering_by_id: HashMap<String, SteeringDirective>,
}

/// Registry managing built-in and custom `.md` / `tags.toml` conversation rubric tags
/// (`ConversationTag`) and inline `#steer:` directives (`SteeringDirective`).
#[derive(Debug)]
pub struct TagRegistry {
    inner: Arc<RwLock<TagRegistryInner>>,
}

impl Clone for TagRegistry {
    fn clone(&self) -> Self {
        Self {
            inner: Arc::clone(&self.inner),
        }
    }
}

impl Default for TagRegistry {
    fn default() -> Self {
        Self::new()
    }
}

impl TagRegistry {
    /// Creates a new [`TagRegistry`] seeded with the 5 built-in conversation rubric tags
    /// and 4 built-in steering directives, and automatically loads custom user tags/manifests
    /// from `$HOME/.config/spell/tags.toml`, `$HOME/.tags/tags.toml`, and `$HOME/.tags/*.md`.
    pub fn new() -> Self {
        let registry = Self {
            inner: Arc::new(RwLock::new(TagRegistryInner::default())),
        };
        for tag in builtin_conversation_tags() {
            registry.upsert_tag(tag);
        }
        for directive in builtin_steering_directives() {
            registry.upsert_steering(directive);
        }
        registry.load_home_tags();
        registry
    }

    /// Creates a new [`TagRegistry`] seeded with built-ins plus custom `.md` and `tags.toml`
    /// definitions from `$HOME` and `<project_root>`.
    pub fn with_project_root(project_root: &Path) -> Self {
        let registry = Self::new();
        registry.load_project_tags(project_root);
        registry
    }

    /// Alias for [`TagRegistry::with_project_root`].
    pub fn from_workspace(project_root: &Path) -> Self {
        Self::with_project_root(project_root)
    }

    /// Alias for [`TagRegistry::load_project_tags`].
    pub fn load_from_workspace(&self, project_root: &Path) -> usize {
        self.load_project_tags(project_root)
    }

    /// Loads custom `tags.toml` and `.md` rubric files from `$HOME/.config/spell/tags.toml`,
    /// `$HOME/.tags/tags.toml`, and `$HOME/.tags/*.md` if `$HOME` (or `USERPROFILE`) is set.
    pub fn load_home_tags(&self) -> usize {
        if let Ok(home) = std::env::var("HOME").or_else(|_| std::env::var("USERPROFILE")) {
            let trimmed = home.trim();
            if !trimmed.is_empty() {
                let home_path = Path::new(trimmed);
                let mut count = 0usize;
                let spell_config_toml = home_path.join(".config").join("spell").join("tags.toml");
                if spell_config_toml.is_file() {
                    count += self.load_tags_toml_file(&spell_config_toml);
                }
                let home_tags_dir = home_path.join(".tags");
                count += self.load_tags_from_dir(&home_tags_dir);
                return count;
            }
        }
        0
    }

    /// Loads custom `tags.toml` and `.md` rubric files from `<project_root>/tags.toml`,
    /// `<project_root>/.tags/tags.toml`, and `<project_root>/.tags/*.md`.
    pub fn load_project_tags(&self, project_root: &Path) -> usize {
        let mut count = 0usize;
        let root_toml = project_root.join("tags.toml");
        if root_toml.is_file() {
            count += self.load_tags_toml_file(&root_toml);
        }
        let dot_tags = project_root.join(".tags");
        if dot_tags.is_dir() {
            count += self.load_tags_from_dir(&dot_tags);
        }
        count
    }

    /// Loads custom `.md` and `tags.toml` files from both `$HOME` and `<project_root>`.
    pub fn load_custom_tags(&self, project_root: Option<&Path>) -> usize {
        let mut count = self.load_home_tags();
        if let Some(root) = project_root {
            count += self.load_project_tags(root);
        }
        count
    }

    /// Parses and registers tags and steering directives from a `tags.toml` file on disk.
    pub fn load_tags_toml_file(&self, path: &Path) -> usize {
        if !path.is_file() {
            return 0;
        }
        match fs::read_to_string(path) {
            Ok(raw) => self.load_tags_toml_str(&raw),
            Err(_) => 0,
        }
    }

    /// Parses and registers tags and steering directives from a raw `tags.toml` string.
    pub fn load_tags_toml_str(&self, raw_toml: &str) -> usize {
        let (tags, steering) = parse_tags_toml(raw_toml);
        let count = tags.len() + steering.len();
        for tag in tags {
            self.upsert_tag(tag);
        }
        for directive in steering {
            self.upsert_steering(directive);
        }
        count
    }

    /// Loads `tags.toml` and `.md` rubric tag files from `dir` (or `dir/.tags` if `dir` is a project root containing `.tags`).
    pub fn load_tags_from_dir(&self, dir: &Path) -> usize {
        let mut loaded = 0usize;
        if dir.file_name().and_then(|s| s.to_str()) != Some(".tags") {
            let root_toml = dir.join("tags.toml");
            if root_toml.is_file() {
                loaded += self.load_tags_toml_file(&root_toml);
            }
        }

        let target_dir = if dir.file_name().and_then(|s| s.to_str()) == Some(".tags") {
            dir.to_path_buf()
        } else if dir.join(".tags").is_dir() {
            dir.join(".tags")
        } else {
            dir.to_path_buf()
        };

        if !target_dir.is_dir() {
            return loaded;
        }

        let dot_tags_toml = target_dir.join("tags.toml");
        if dot_tags_toml.is_file() {
            loaded += self.load_tags_toml_file(&dot_tags_toml);
        }

        let entries = match fs::read_dir(&target_dir) {
            Ok(e) => e,
            Err(_) => return loaded,
        };

        let mut paths = Vec::new();
        for entry in entries.flatten() {
            let p = entry.path();
            if p.is_file() && p.extension().and_then(|s| s.to_str()) == Some("md") {
                paths.push(p);
            }
        }
        paths.sort();

        for path in paths {
            if let Ok(content) = fs::read_to_string(&path) {
                if let Some(tag) = parse_markdown_tag_file(&path, &content) {
                    self.upsert_tag(tag);
                    loaded += 1;
                }
            }
        }
        loaded
    }

    /// Returns all registered [`ConversationTag`] entries in deterministic order.
    pub fn list_tags(&self) -> Vec<ConversationTag> {
        let guard = self.inner.read();
        let mut out = Vec::with_capacity(guard.order.len());
        for id in &guard.order {
            if let Some(tag) = guard.tags_by_id.get(id) {
                out.push(tag.clone());
            }
        }
        out
    }

    /// Looks up a [`ConversationTag`] by `id`, `name`, or `trigger` (case-insensitive, hyphen/underscore normalized).
    pub fn get_tag(&self, name_or_id: &str) -> Option<ConversationTag> {
        let trimmed = name_or_id.trim();
        if trimmed.is_empty() {
            return None;
        }
        let stripped = trimmed
            .strip_prefix("@tag:")
            .or_else(|| trimmed.strip_prefix("@skill:"))
            .unwrap_or(trimmed);
        let key = normalize_tag_key(stripped);

        let guard = self.inner.read();
        if let Some(tag) = guard.tags_by_id.get(&key) {
            return Some(tag.clone());
        }
        for id in &guard.order {
            if let Some(t) = guard.tags_by_id.get(id) {
                if normalize_tag_key(&t.id) == key || normalize_tag_key(&t.name) == key {
                    return Some(t.clone());
                }
                if let Some(ref trig) = t.trigger {
                    let trig_trim = trig.trim();
                    if trig_trim.eq_ignore_ascii_case(trimmed) {
                        return Some(t.clone());
                    }
                    let trig_slug = trig_trim
                        .strip_prefix("@tag:")
                        .or_else(|| trig_trim.strip_prefix("@skill:"))
                        .unwrap_or(trig_trim);
                    if normalize_tag_key(trig_slug) == key {
                        return Some(t.clone());
                    }
                }
            }
        }
        None
    }

    /// Inserts or updates a [`ConversationTag`] in the registry.
    pub fn upsert_tag(&self, mut tag: ConversationTag) {
        let canonical_id = if tag.id.trim().is_empty() {
            normalize_tag_key(&tag.name)
        } else {
            normalize_tag_key(&tag.id)
        };
        if tag.name.trim().is_empty() {
            tag.name = canonical_id.clone();
        }
        if tag.category.trim().is_empty() {
            tag.category = "general".to_string();
        }
        tag.id = canonical_id.clone();

        let mut guard = self.inner.write();
        if !guard.tags_by_id.contains_key(&canonical_id) {
            guard.order.push(canonical_id.clone());
        }
        guard.tags_by_id.insert(canonical_id, tag);
    }

    /// Removes a [`ConversationTag`] by `name_or_id` from the registry (and active list), returning it if found.
    pub fn remove_tag(&self, name_or_id: &str) -> Option<ConversationTag> {
        let key = normalize_tag_key(
            name_or_id
                .trim()
                .strip_prefix("@tag:")
                .or_else(|| name_or_id.trim().strip_prefix("@skill:"))
                .unwrap_or(name_or_id.trim()),
        );
        let mut guard = self.inner.write();
        let target_key = if guard.tags_by_id.contains_key(&key) {
            Some(key)
        } else {
            guard
                .tags_by_id
                .iter()
                .find(|(_, t)| {
                    normalize_tag_key(&t.id) == key || normalize_tag_key(&t.name) == key
                })
                .map(|(k, _)| k.clone())
        };

        if let Some(k) = target_key {
            guard.order.retain(|id| id != &k);
            guard.active_tag_ids.retain(|id| id != &k);
            guard.tags_by_id.remove(&k)
        } else {
            None
        }
    }

    /// Returns all registered [`SteeringDirective`] entries in deterministic order.
    pub fn list_steering_directives(&self) -> Vec<SteeringDirective> {
        let guard = self.inner.read();
        let mut out = Vec::with_capacity(guard.steering_order.len());
        for id in &guard.steering_order {
            if let Some(d) = guard.steering_by_id.get(id) {
                out.push(d.clone());
            }
        }
        out
    }

    /// Looks up a [`SteeringDirective`] by `id`, `name`, alias, or `#steer:` / `#spell` trigger.
    pub fn get_steering(&self, name_or_trigger: &str) -> Option<SteeringDirective> {
        let trimmed = name_or_trigger.trim();
        if trimmed.is_empty() {
            return None;
        }
        let stripped = trimmed
            .strip_prefix("#steer:")
            .or_else(|| trimmed.strip_prefix('#'))
            .unwrap_or(trimmed);
        let key = normalize_tag_key(stripped);

        let guard = self.inner.read();
        if let Some(d) = guard.steering_by_id.get(&key) {
            return Some(d.clone());
        }
        for id in &guard.steering_order {
            if let Some(d) = guard.steering_by_id.get(id) {
                if normalize_tag_key(&d.id) == key || normalize_tag_key(&d.name) == key {
                    return Some(d.clone());
                }
                let trig_trim = d.trigger.trim();
                if !trig_trim.is_empty() {
                    if trig_trim.eq_ignore_ascii_case(trimmed) {
                        return Some(d.clone());
                    }
                    let trig_slug = trig_trim
                        .strip_prefix("#steer:")
                        .or_else(|| trig_trim.strip_prefix('#'))
                        .unwrap_or(trig_trim);
                    if normalize_tag_key(trig_slug) == key {
                        return Some(d.clone());
                    }
                }
            }
        }
        // Check built-in aliases (`think` / `deep_think` -> `deep-think`, `fast` / `fast_patch` / `no-think` / `no_think` -> `fast-patch`, `spell` -> `spell`)
        match key.as_str() {
            "think" | "deep-think" => guard.steering_by_id.get("deep-think").cloned(),
            "fast" | "fast-patch" | "no-think" => guard.steering_by_id.get("fast-patch").cloned(),
            "spell" => guard.steering_by_id.get("spell").cloned(),
            _ => None,
        }
    }

    /// Inserts or updates a [`SteeringDirective`] in the registry.
    pub fn upsert_steering(&self, mut directive: SteeringDirective) {
        let canonical_id = if directive.id.trim().is_empty() {
            normalize_tag_key(&directive.name)
        } else {
            normalize_tag_key(&directive.id)
        };
        if directive.name.trim().is_empty() {
            directive.name = canonical_id.clone();
        }
        if directive.trigger.trim().is_empty() {
            directive.trigger = format!("#steer:{canonical_id}");
        }
        directive.id = canonical_id.clone();

        let mut guard = self.inner.write();
        if !guard.steering_by_id.contains_key(&canonical_id) {
            guard.steering_order.push(canonical_id.clone());
        }
        guard.steering_by_id.insert(canonical_id, directive);
    }

    /// Preprocesses `raw_prompt` using this registry to extract inline `@tag:`, `@skill:`, and `#steer:` tokens.
    pub fn preprocess_prompt(&self, raw_prompt: &str) -> PreprocessedPrompt {
        preprocess_prompt_tags(raw_prompt, self)
    }

    /// Activates/applies a tag by `name_or_id` for the current conversation session.
    pub fn apply_tag(&self, name_or_id: &str) -> Option<ConversationTag> {
        let tag = self.get_tag(name_or_id)?;
        let key = normalize_tag_key(&tag.id);
        let mut guard = self.inner.write();
        if !guard.active_tag_ids.contains(&key) {
            guard.active_tag_ids.push(key);
        }
        Some(tag)
    }

    /// Deactivates a previously applied tag by `name_or_id` without deleting it from the registry.
    pub fn deactivate_tag(&self, name_or_id: &str) -> bool {
        let key = normalize_tag_key(name_or_id);
        let mut guard = self.inner.write();
        let before = guard.active_tag_ids.len();
        guard
            .active_tag_ids
            .retain(|id| id != &key && normalize_tag_key(id) != key);
        guard.active_tag_ids.len() < before
    }

    /// Returns the list of currently active tag IDs.
    pub fn active_tag_names(&self) -> Vec<String> {
        self.inner.read().active_tag_ids.clone()
    }

    /// Returns the resolved [`ConversationTag`] structs for all currently active tags.
    pub fn active_tags(&self) -> Vec<ConversationTag> {
        let names = self.active_tag_names();
        names
            .iter()
            .filter_map(|name| self.get_tag(name))
            .collect()
    }

    /// Formats `<conversation_rubric_tags>...</conversation_rubric_tags>` for the specified
    /// `active_tag_names`, ready for injection into the system prompt or conversation context.
    pub fn format_active_tags_rubric_block(&self, active_tag_names: &[String]) -> String {
        let mut resolved = Vec::new();
        for name in active_tag_names {
            if let Some(tag) = self.get_tag(name) {
                if !resolved.iter().any(|t: &ConversationTag| t.id == tag.id) {
                    resolved.push(tag);
                }
            }
        }
        format_tags_rubric_block(&resolved)
    }
}

/// Formats a slice of [`ConversationTag`] items into a `<conversation_rubric_tags>...</conversation_rubric_tags>`
/// block for system prompt or conversation context injection. Returns an empty string if `tags` is empty.
pub fn format_tags_rubric_block(tags: &[ConversationTag]) -> String {
    if tags.is_empty() {
        return String::new();
    }
    let mut out = String::from("<conversation_rubric_tags>\n");
    for tag in tags {
        out.push_str(&format!(
            "<tag id=\"{}\" name=\"{}\" category=\"{}\" builtin=\"{}\">\n",
            tag.id, tag.name, tag.category, tag.is_builtin
        ));
        if !tag.description.trim().is_empty() {
            out.push_str(&format!("Description: {}\n", tag.description.trim()));
        }
        out.push_str(tag.rubric_prompt.trim());
        out.push_str("\n</tag>\n");
    }
    out.push_str("</conversation_rubric_tags>");
    out
}

// ============================================================================
// `tags.toml` Manifest Parser & Markdown Frontmatter Parser
// ============================================================================

fn strip_toml_line_comment(line: &str) -> &str {
    let mut in_double = false;
    let mut in_single = false;
    let mut escaped = false;
    for (idx, ch) in line.char_indices() {
        if escaped {
            escaped = false;
            continue;
        }
        if ch == '\\' && in_double {
            escaped = true;
            continue;
        }
        if ch == '"' && !in_single {
            in_double = !in_double;
        } else if ch == '\'' && !in_double {
            in_single = !in_single;
        } else if ch == '#' && !in_double && !in_single {
            return &line[..idx];
        }
    }
    line
}

fn unquote_toml_string(raw: &str) -> String {
    let t = raw.trim();
    if let Some(inner) = t.strip_prefix("\"\"\"").and_then(|s| s.strip_suffix("\"\"\"")) {
        return inner.trim().to_string();
    }
    if let Some(inner) = t.strip_prefix("'''").and_then(|s| s.strip_suffix("'''")) {
        return inner.trim().to_string();
    }
    if let Some(inner) = t.strip_prefix('"').and_then(|s| s.strip_suffix('"')) {
        let mut out = String::with_capacity(inner.len());
        let mut chars = inner.chars();
        while let Some(c) = chars.next() {
            if c == '\\' {
                match chars.next() {
                    Some('n') => out.push('\n'),
                    Some('r') => out.push('\r'),
                    Some('t') => out.push('\t'),
                    Some('"') => out.push('"'),
                    Some('\\') => out.push('\\'),
                    Some(other) => {
                        out.push('\\');
                        out.push(other);
                    }
                    None => out.push('\\'),
                }
            } else {
                out.push(c);
            }
        }
        return out;
    }
    if let Some(inner) = t.strip_prefix('\'').and_then(|s| s.strip_suffix('\'')) {
        return inner.to_string();
    }
    t.to_string()
}

fn parse_string_list_value(raw: &str) -> Vec<String> {
    let trimmed = raw.trim();
    let inner = trimmed
        .strip_prefix('[')
        .and_then(|s| s.strip_suffix(']'))
        .unwrap_or(trimmed);
    inner
        .split(',')
        .map(unquote_toml_string)
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .collect()
}

fn parse_bool_value(raw: &str) -> Option<bool> {
    match unquote_toml_string(raw).trim().to_ascii_lowercase().as_str() {
        "true" | "yes" | "1" => Some(true),
        "false" | "no" | "0" => Some(false),
        _ => None,
    }
}

fn parse_inline_table_thinking(raw: &str) -> (Option<bool>, Option<u64>) {
    let trimmed = raw.trim();
    let inner = trimmed
        .strip_prefix('{')
        .and_then(|s| s.strip_suffix('}'))
        .unwrap_or(trimmed);
    let mut enable_thinking = None;
    let mut thinking_budget = None;
    for pair in inner.split(',') {
        let sep = if pair.contains('=') { '=' } else { ':' };
        if let Some((k, v)) = pair.split_once(sep) {
            let key = unquote_toml_string(k).trim().to_ascii_lowercase();
            let val = v.trim();
            match key.as_str() {
                "enable_thinking" | "enablethinking" => {
                    enable_thinking = parse_bool_value(val);
                }
                "thinking_budget" | "thinkingbudget" => {
                    thinking_budget = unquote_toml_string(val).trim().parse::<u64>().ok();
                }
                _ => {}
            }
        }
    }
    (enable_thinking, thinking_budget)
}

/// Parses a `tags.toml` manifest into `(`[`Vec<ConversationTag>`]`, `[`Vec<SteeringDirective>`]`)`.
///
/// Supports `[tags.<name>]`, `[skills.<name>]`, and `[steering.<name>]` sections with
/// tool scoping (`allowed_tools`), sampling parameters (`temperature`, `top_p`, `presence_penalty`,
/// `repetition_penalty`, `max_tokens`), thinking controls (`chat_template_kwargs`, `enable_thinking`,
/// `thinking_budget`), and prompt instructions (`system_append`, `rubric_prompt`).
pub fn parse_tags_toml(raw_toml: &str) -> (Vec<ConversationTag>, Vec<SteeringDirective>) {
    #[derive(Clone, Copy, PartialEq, Eq)]
    enum SectionKind {
        Tag,
        Skill,
        Steering,
    }

    let mut tags = Vec::new();
    let mut steering = Vec::new();

    let mut current_section: Option<(SectionKind, String, HashMap<String, String>)> = None;

    let flush_section = |sec: (SectionKind, String, HashMap<String, String>),
                         tags_out: &mut Vec<ConversationTag>,
                         steering_out: &mut Vec<SteeringDirective>| {
        let (kind, raw_name, map) = sec;
        let clean_name = raw_name.trim().trim_matches('"').trim_matches('\'').to_string();
        let canonical_id = map
            .get("id")
            .map(|s| normalize_tag_key(&unquote_toml_string(s)))
            .unwrap_or_else(|| normalize_tag_key(&clean_name));
        let display_name = map
            .get("name")
            .map(|s| unquote_toml_string(s))
            .unwrap_or_else(|| clean_name.clone());
        let description = map
            .get("description")
            .or_else(|| map.get("summary"))
            .map(|s| unquote_toml_string(s))
            .unwrap_or_default();
        let prompt_text = map
            .get("rubric_prompt")
            .or_else(|| map.get("system_append"))
            .or_else(|| map.get("prompt"))
            .map(|s| unquote_toml_string(s))
            .unwrap_or_default();

        let temperature = map
            .get("temperature")
            .and_then(|v| unquote_toml_string(v).trim().parse::<f64>().ok());
        let top_p = map
            .get("top_p")
            .and_then(|v| unquote_toml_string(v).trim().parse::<f64>().ok());
        let presence_penalty = map
            .get("presence_penalty")
            .and_then(|v| unquote_toml_string(v).trim().parse::<f64>().ok());
        let repetition_penalty = map
            .get("repetition_penalty")
            .and_then(|v| unquote_toml_string(v).trim().parse::<f64>().ok());
        let max_tokens = map
            .get("max_tokens")
            .and_then(|v| unquote_toml_string(v).trim().parse::<u64>().ok());

        let (kw_thinking, kw_budget) = map
            .get("chat_template_kwargs")
            .map(|v| parse_inline_table_thinking(v))
            .unwrap_or((None, None));
        let enable_thinking = map
            .get("enable_thinking")
            .and_then(|v| parse_bool_value(v))
            .or(kw_thinking);
        let thinking_budget = map
            .get("thinking_budget")
            .and_then(|v| unquote_toml_string(v).trim().parse::<u64>().ok())
            .or(kw_budget);

        match kind {
            SectionKind::Tag | SectionKind::Skill => {
                let default_cat = if kind == SectionKind::Skill {
                    "engineering"
                } else {
                    "general"
                };
                let category = map
                    .get("category")
                    .map(|s| unquote_toml_string(s).to_lowercase())
                    .unwrap_or_else(|| default_cat.to_string());
                let trigger = map
                    .get("trigger")
                    .map(|s| unquote_toml_string(s))
                    .or_else(|| {
                        if kind == SectionKind::Skill {
                            Some(format!("@skill:{canonical_id}"))
                        } else {
                            Some(format!("@tag:{canonical_id}"))
                        }
                    });
                let allowed_tools = map
                    .get("allowed_tools")
                    .map(|s| parse_string_list_value(s))
                    .filter(|v| !v.is_empty());

                tags_out.push(ConversationTag {
                    id: canonical_id,
                    name: display_name,
                    category,
                    description,
                    rubric_prompt: prompt_text,
                    is_builtin: false,
                    trigger,
                    allowed_tools,
                    temperature,
                    top_p,
                    presence_penalty,
                    repetition_penalty,
                    max_tokens,
                    enable_thinking,
                    thinking_budget,
                });
            }
            SectionKind::Steering => {
                let trigger = map
                    .get("trigger")
                    .map(|s| unquote_toml_string(s))
                    .unwrap_or_else(|| format!("#steer:{canonical_id}"));
                steering_out.push(SteeringDirective {
                    id: canonical_id,
                    name: display_name,
                    trigger,
                    description,
                    system_append: prompt_text,
                    temperature,
                    top_p,
                    presence_penalty,
                    repetition_penalty,
                    max_tokens,
                    enable_thinking,
                    thinking_budget,
                    is_builtin: false,
                });
            }
        }
    };

    let mut lines = raw_toml.lines().peekable();
    while let Some(raw_line) = lines.next() {
        let line = strip_toml_line_comment(raw_line).trim();
        if line.is_empty() {
            continue;
        }

        if let Some(header) = line.strip_prefix('[').and_then(|s| s.strip_suffix(']')) {
            if let Some(prev) = current_section.take() {
                flush_section(prev, &mut tags, &mut steering);
            }
            let header_trim = header.trim();
            if let Some(rest) = header_trim.strip_prefix("tags.") {
                current_section = Some((SectionKind::Tag, rest.to_string(), HashMap::new()));
            } else if let Some(rest) = header_trim.strip_prefix("skills.") {
                current_section = Some((SectionKind::Skill, rest.to_string(), HashMap::new()));
            } else if let Some(rest) = header_trim.strip_prefix("steering.") {
                current_section = Some((SectionKind::Steering, rest.to_string(), HashMap::new()));
            }
            continue;
        }

        if let Some((_, _, ref mut map)) = current_section {
            if let Some((k, v)) = line.split_once('=') {
                let key = k.trim().to_ascii_lowercase();
                let mut val_acc = v.trim().to_string();

                // Support multi-line arrays `[ ... ]` or multi-line triple-quoted strings `""" ... """`
                if (val_acc.starts_with('[') && !val_acc.ends_with(']'))
                    || (val_acc.starts_with('{') && !val_acc.ends_with('}'))
                {
                    let close_char = if val_acc.starts_with('[') { ']' } else { '}' };
                    while let Some(next_raw) = lines.next() {
                        let next_clean = strip_toml_line_comment(next_raw).trim();
                        val_acc.push(' ');
                        val_acc.push_str(next_clean);
                        if next_clean.ends_with(close_char) {
                            break;
                        }
                    }
                } else if val_acc.starts_with("\"\"\"") && val_acc.len() >= 3 && !val_acc[3..].contains("\"\"\"") {
                    while let Some(next_raw) = lines.next() {
                        val_acc.push('\n');
                        val_acc.push_str(next_raw);
                        if next_raw.contains("\"\"\"") {
                            break;
                        }
                    }
                }

                map.insert(key, val_acc);
            }
        }
    }

    if let Some(last) = current_section.take() {
        flush_section(last, &mut tags, &mut steering);
    }

    (tags, steering)
}

/// Parses a custom `.md` rubric file into a [`ConversationTag`].
///
/// Supports optional YAML frontmatter delimited by `---` lines (`id`, `name`, `category`,
/// `description`, `trigger`, `allowed_tools`, `temperature`, `top_p`, `presence_penalty`,
/// `repetition_penalty`, `max_tokens`, `enable_thinking`, `thinking_budget`)
/// as well as plain Markdown files (deriving `id`/`name` from the filename stem).
pub fn parse_markdown_tag_file(path: &Path, content: &str) -> Option<ConversationTag> {
    let trimmed = content.trim();
    if trimmed.is_empty() {
        return None;
    }

    let file_stem = path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("custom-tag");
    let default_id = normalize_tag_key(file_stem);

    let mut id = default_id.clone();
    let mut name = file_stem.to_string();
    let mut category = "general".to_string();
    let mut description = String::new();
    let mut rubric_body = trimmed.to_string();
    let mut trigger: Option<String> = None;
    let mut allowed_tools: Option<Vec<String>> = None;
    let mut temperature: Option<f64> = None;
    let mut top_p: Option<f64> = None;
    let mut presence_penalty: Option<f64> = None;
    let mut repetition_penalty: Option<f64> = None;
    let mut max_tokens: Option<u64> = None;
    let mut enable_thinking: Option<bool> = None;
    let mut thinking_budget: Option<u64> = None;

    if let Some(after_open) = trimmed.strip_prefix("---") {
        if let Some(end_idx) = after_open.find("\n---") {
            let frontmatter = &after_open[..end_idx];
            let remainder = &after_open[end_idx + 4..];
            rubric_body = remainder.trim().to_string();

            let mut in_allowed_tools_list = false;
            for line in frontmatter.lines() {
                let line_trim = line.trim();
                if in_allowed_tools_list {
                    if let Some(item) = line_trim.strip_prefix('-') {
                        let tool_name = unquote_toml_string(item.trim());
                        if !tool_name.is_empty() {
                            allowed_tools
                                .get_or_insert_with(Vec::new)
                                .push(tool_name);
                        }
                        continue;
                    } else if !line_trim.is_empty() {
                        in_allowed_tools_list = false;
                    }
                }

                if let Some((k, v)) = line.split_once(':') {
                    let key = k.trim().to_lowercase();
                    let raw_val = v.trim();
                    if key == "allowed_tools" && raw_val.is_empty() {
                        in_allowed_tools_list = true;
                        continue;
                    }
                    let val = raw_val.trim_matches('"').trim_matches('\'').to_string();
                    if val.is_empty() {
                        continue;
                    }
                    match key.as_str() {
                        "id" => id = normalize_tag_key(&val),
                        "name" => name = val,
                        "category" => category = val.to_lowercase(),
                        "description" | "summary" => description = val,
                        "trigger" => trigger = Some(val),
                        "allowed_tools" => {
                            let parsed_list = parse_string_list_value(raw_val);
                            if !parsed_list.is_empty() {
                                allowed_tools = Some(parsed_list);
                            }
                        }
                        "temperature" => temperature = val.parse::<f64>().ok(),
                        "top_p" => top_p = val.parse::<f64>().ok(),
                        "presence_penalty" => presence_penalty = val.parse::<f64>().ok(),
                        "repetition_penalty" => repetition_penalty = val.parse::<f64>().ok(),
                        "max_tokens" => max_tokens = val.parse::<u64>().ok(),
                        "enable_thinking" => enable_thinking = parse_bool_value(&val),
                        "thinking_budget" => thinking_budget = val.parse::<u64>().ok(),
                        _ => {}
                    }
                }
            }
        }
    }

    if description.is_empty() {
        for line in rubric_body.lines() {
            let clean = line.trim().trim_start_matches('#').trim();
            if !clean.is_empty() {
                description = clean.chars().take(160).collect();
                break;
            }
        }
    }

    if rubric_body.is_empty() {
        rubric_body = description.clone();
    }

    Some(ConversationTag {
        id,
        name,
        category,
        description,
        rubric_prompt: rubric_body,
        is_builtin: false,
        trigger,
        allowed_tools,
        temperature,
        top_p,
        presence_penalty,
        repetition_penalty,
        max_tokens,
        enable_thinking,
        thinking_budget,
    })
}

// ============================================================================
// Deterministic Inline Tag & Steering Lexer (`preprocess_prompt_tags`)
// ============================================================================

fn is_slug_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '-' || c == '_'
}

fn match_inline_prefix(s: &str) -> Option<(&'static str, usize)> {
    let bytes = s.as_bytes();
    if bytes.len() > 5 && bytes[..5].eq_ignore_ascii_case(b"@tag:") {
        Some(("@tag:", 5))
    } else if bytes.len() > 7 && bytes[..7].eq_ignore_ascii_case(b"@skill:") {
        Some(("@skill:", 7))
    } else if bytes.len() > 7 && bytes[..7].eq_ignore_ascii_case(b"#steer:") {
        Some(("#steer:", 7))
    } else {
        None
    }
}

/// Scans `raw_prompt` while tracking fenced code blocks (```` ``` ````) and inline backtick
/// code spans (`` `...` ``) so `@tag:<slug>`, `@skill:<slug>`, and `#steer:<slug>` tokens inside
/// code blocks/spans are never stripped or triggered.
pub fn preprocess_prompt_tags(raw_prompt: &str, registry: &TagRegistry) -> PreprocessedPrompt {
    let mut cleaned = String::with_capacity(raw_prompt.len());
    let mut matched_tags: Vec<ConversationTag> = Vec::new();
    let mut matched_steering: Vec<SteeringDirective> = Vec::new();

    let bytes = raw_prompt.as_bytes();
    let len = raw_prompt.len();
    let mut i = 0usize;

    while i < len {
        // 1. Check for backtick code spans or fenced code blocks (`...` or ```...```)
        if bytes[i] == b'`' {
            let mut tick_count = 0usize;
            while i + tick_count < len && bytes[i + tick_count] == b'`' {
                tick_count += 1;
            }
            cleaned.push_str(&raw_prompt[i..i + tick_count]);
            i += tick_count;

            if tick_count >= 3 {
                // Fenced code block: copy verbatim until a closing sequence of >= tick_count backticks
                while i < len {
                    if bytes[i] == b'`' {
                        let mut close_ticks = 0usize;
                        while i + close_ticks < len && bytes[i + close_ticks] == b'`' {
                            close_ticks += 1;
                        }
                        cleaned.push_str(&raw_prompt[i..i + close_ticks]);
                        i += close_ticks;
                        if close_ticks >= tick_count {
                            break;
                        }
                    } else {
                        let ch = raw_prompt[i..].chars().next().unwrap();
                        cleaned.push(ch);
                        i += ch.len_utf8();
                    }
                }
            } else {
                // Inline code span: copy verbatim until matching `tick_count` backticks
                while i < len {
                    if bytes[i] == b'`' {
                        let mut close_ticks = 0usize;
                        while i + close_ticks < len && bytes[i + close_ticks] == b'`' {
                            close_ticks += 1;
                        }
                        cleaned.push_str(&raw_prompt[i..i + close_ticks]);
                        i += close_ticks;
                        if close_ticks == tick_count {
                            break;
                        }
                    } else {
                        let ch = raw_prompt[i..].chars().next().unwrap();
                        cleaned.push(ch);
                        i += ch.len_utf8();
                    }
                }
            }
            continue;
        }

        // 2. Outside code spans: check for `@tag:<slug>`, `@skill:<slug>`, or `#steer:<slug>`
        let at_word_boundary = if i == 0 {
            true
        } else {
            let prev_ch = raw_prompt[..i].chars().next_back().unwrap_or(' ');
            !is_slug_char(prev_ch)
        };

        if at_word_boundary {
            if bytes.len() - i >= 6
                && bytes[i..i + 6].eq_ignore_ascii_case(b"#spell")
                && (i + 6 >= len || !is_slug_char(raw_prompt[i + 6..].chars().next().unwrap()))
            {
                if let Some(directive) = registry
                    .get_steering("#spell")
                    .or_else(|| registry.get_steering("spell"))
                {
                    if !matched_steering.iter().any(|d| d.id == directive.id) {
                        matched_steering.push(directive);
                    }
                    i += 6;
                    let at_line_start = cleaned.is_empty() || cleaned.ends_with('\n');
                    let preceded_by_space = cleaned.ends_with(' ') || cleaned.ends_with('\t');
                    if at_line_start || preceded_by_space {
                        while i < len && (bytes[i] == b' ' || bytes[i] == b'\t') {
                            i += 1;
                        }
                    }
                    if i >= len || bytes[i] == b'\n' || bytes[i] == b'\r' {
                        while cleaned.ends_with(' ') || cleaned.ends_with('\t') {
                            cleaned.pop();
                        }
                    }
                    continue;
                }
            }

            if let Some((prefix_kind, prefix_len)) = match_inline_prefix(&raw_prompt[i..]) {
                let slug_start = i + prefix_len;
                let mut slug_end = slug_start;
                while slug_end < len {
                    let ch = raw_prompt[slug_end..].chars().next().unwrap();
                    if is_slug_char(ch) {
                        slug_end += ch.len_utf8();
                    } else {
                        break;
                    }
                }

                if slug_end > slug_start {
                    let full_token = &raw_prompt[i..slug_end];
                    let slug = &raw_prompt[slug_start..slug_end];

                    let mut recognized = false;
                    if prefix_kind == "#steer:" {
                        if let Some(directive) = registry
                            .get_steering(full_token)
                            .or_else(|| registry.get_steering(slug))
                        {
                            if !matched_steering.iter().any(|d| d.id == directive.id) {
                                matched_steering.push(directive);
                            }
                            recognized = true;
                        }
                    } else if let Some(tag) = registry
                        .get_tag(full_token)
                        .or_else(|| registry.get_tag(slug))
                    {
                        if !matched_tags.iter().any(|t| t.id == tag.id) {
                            matched_tags.push(tag);
                        }
                        recognized = true;
                    }

                    if recognized {
                        i = slug_end;
                        // Normalize horizontal whitespace around the stripped token
                        let at_line_start = cleaned.is_empty() || cleaned.ends_with('\n');
                        let preceded_by_space = cleaned.ends_with(' ') || cleaned.ends_with('\t');
                        if at_line_start || preceded_by_space {
                            while i < len && (bytes[i] == b' ' || bytes[i] == b'\t') {
                                i += 1;
                            }
                        }
                        if i >= len || bytes[i] == b'\n' || bytes[i] == b'\r' {
                            while cleaned.ends_with(' ') || cleaned.ends_with('\t') {
                                cleaned.pop();
                            }
                        }
                        continue;
                    }
                }
            }
        }

        let ch = raw_prompt[i..].chars().next().unwrap();
        cleaned.push(ch);
        i += ch.len_utf8();
    }

    let mut allowed_tools: Option<Vec<String>> = None;
    let mut temperature: Option<f64> = None;
    let mut top_p: Option<f64> = None;
    let mut presence_penalty: Option<f64> = None;
    let mut repetition_penalty: Option<f64> = None;
    let mut max_tokens: Option<u64> = None;
    let mut enable_thinking: Option<bool> = None;
    let mut thinking_budget: Option<u64> = None;
    let mut tail_parts: Vec<String> = Vec::new();

    for tag in &matched_tags {
        if let Some(ref tools) = tag.allowed_tools {
            let entry = allowed_tools.get_or_insert_with(Vec::new);
            for t in tools {
                if !entry.contains(t) {
                    entry.push(t.clone());
                }
            }
        }
        if tag.temperature.is_some() {
            temperature = tag.temperature;
        }
        if tag.top_p.is_some() {
            top_p = tag.top_p;
        }
        if tag.presence_penalty.is_some() {
            presence_penalty = tag.presence_penalty;
        }
        if tag.repetition_penalty.is_some() {
            repetition_penalty = tag.repetition_penalty;
        }
        if tag.max_tokens.is_some() {
            max_tokens = tag.max_tokens;
        }
        if tag.enable_thinking.is_some() {
            enable_thinking = tag.enable_thinking;
        }
        if tag.thinking_budget.is_some() {
            thinking_budget = tag.thinking_budget;
        }
        let rp = tag.rubric_prompt.trim();
        if !rp.is_empty() {
            tail_parts.push(rp.to_string());
        }
    }

    for steer in &matched_steering {
        if steer.temperature.is_some() {
            temperature = steer.temperature;
        }
        if steer.top_p.is_some() {
            top_p = steer.top_p;
        }
        if steer.presence_penalty.is_some() {
            presence_penalty = steer.presence_penalty;
        }
        if steer.repetition_penalty.is_some() {
            repetition_penalty = steer.repetition_penalty;
        }
        if steer.max_tokens.is_some() {
            max_tokens = steer.max_tokens;
        }
        if steer.enable_thinking.is_some() {
            enable_thinking = steer.enable_thinking;
        }
        if steer.thinking_budget.is_some() {
            thinking_budget = steer.thinking_budget;
        }
        let sa = steer.system_append.trim();
        if !sa.is_empty() {
            tail_parts.push(sa.to_string());
        }
    }

    let chat_template_kwargs = enable_thinking.map(|val| {
        serde_json::json!({
            "enable_thinking": val
        })
    });

    PreprocessedPrompt {
        raw_prompt: raw_prompt.to_string(),
        cleaned_prompt: cleaned.trim().to_string(),
        matched_tags,
        matched_steering,
        allowed_tools,
        temperature,
        top_p,
        presence_penalty,
        repetition_penalty,
        max_tokens,
        enable_thinking,
        thinking_budget,
        chat_template_kwargs,
        tail_system_append: tail_parts.join("\n"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn test_builtin_conversation_tags_and_lookup() {
        let registry = TagRegistry::new();
        let all = registry.list_tags();
        assert!(all.len() >= 5);

        let expected = [
            ("research-synthesis", "research"),
            ("executive-decision-memo", "writing"),
            ("critical-rubric-evaluator", "analysis"),
            ("systems-architecture", "engineering"),
            ("editorial-clarity", "writing"),
        ];

        for (id, cat) in expected {
            let tag = registry.get_tag(id).expect("built-in tag must exist");
            assert_eq!(tag.id, id);
            assert_eq!(tag.category, cat);
            assert!(tag.is_builtin);
            assert!(!tag.description.is_empty());
            assert!(!tag.rubric_prompt.is_empty());
        }

        let block = registry.format_active_tags_rubric_block(&[
            "research-synthesis".to_string(),
            "systems-architecture".to_string(),
        ]);
        assert!(block.starts_with("<conversation_rubric_tags>"));
        assert!(block.ends_with("</conversation_rubric_tags>"));
        assert!(block.contains("id=\"research-synthesis\""));
        assert!(block.contains("id=\"systems-architecture\""));
    }

    #[test]
    fn test_custom_md_tag_loading_and_crud() {
        let tmp_dir = std::env::temp_dir().join(format!("spell_tags_test_{}", std::process::id()));
        let tags_dir = tmp_dir.join(".tags");
        let _ = fs::remove_dir_all(&tmp_dir);
        fs::create_dir_all(&tags_dir).unwrap();

        let md_content = "---\nid: governance-audit\nname: Governance Audit\ncategory: governance\ndescription: Evaluates compliance and policy controls.\nallowed_tools: [\"read_file\", \"grep\"]\ntemperature: 0.4\nenable_thinking: true\n---\n### Governance Audit Rubric\nVerify traceability and audit logs.\n";
        fs::write(tags_dir.join("governance-audit.md"), md_content).unwrap();

        let registry = TagRegistry::with_project_root(&tmp_dir);
        let custom = registry
            .get_tag("governance-audit")
            .expect("custom .md tag should load");
        assert_eq!(custom.name, "Governance Audit");
        assert_eq!(custom.category, "governance");
        assert!(!custom.is_builtin);
        assert!(custom.rubric_prompt.contains("Verify traceability"));
        assert_eq!(
            custom.allowed_tools,
            Some(vec!["read_file".to_string(), "grep".to_string()])
        );
        assert_eq!(custom.temperature, Some(0.4));
        assert_eq!(custom.enable_thinking, Some(true));

        let removed = registry.remove_tag("governance-audit");
        assert!(removed.is_some());
        assert!(registry.get_tag("governance-audit").is_none());

        let _ = fs::remove_dir_all(&tmp_dir);
    }

    #[test]
    fn test_tag_steer_lexer_toml_and_sampling_wire() {
        let toml_spec = r##"
[skills.rust_refactor]
trigger = "@skill:refactor"
description = "Safe Rust AST refactoring with cargo check verification"
allowed_tools = ["ast_query", "apply_unified_diff", "run_cargo_check"]
system_append = "Use AST-aware surgical replacements and verify with cargo check."

[steering.deep_think]
trigger = "#steer:think"
chat_template_kwargs = { enable_thinking = true }
temperature = 0.6
top_p = 0.95
presence_penalty = 0.0

[steering.fast_patch]
trigger = "#steer:fast"
chat_template_kwargs = { enable_thinking = false }
temperature = 0.7
top_p = 0.80
presence_penalty = 1.5
system_append = "Output only unified diffs or exact replacement blocks. Zero prose."
"##;

        let registry = TagRegistry::new();
        let loaded = registry.load_tags_toml_str(toml_spec);
        assert_eq!(loaded, 3);

        let raw_prompt = "@skill:refactor #steer:fast Convert `foo @skill:refactor` and:\n```\n#steer:think\n```\nto a state machine.";
        let prep = preprocess_prompt_tags(raw_prompt, &registry);

        // 1. Outside tokens stripped; inline code span and fenced code block tokens preserved
        assert_eq!(
            prep.cleaned_prompt,
            "Convert `foo @skill:refactor` and:\n```\n#steer:think\n```\nto a state machine."
        );
        assert_eq!(prep.matched_tags.len(), 1);
        assert_eq!(prep.matched_steering.len(), 1);

        // 2. Tool scoping whitelist from `[skills.rust_refactor]`
        assert_eq!(
            prep.allowed_tools,
            Some(vec![
                "ast_query".to_string(),
                "apply_unified_diff".to_string(),
                "run_cargo_check".to_string(),
            ])
        );

        // 3. Sampling & thinking wire parameters from `#steer:fast`
        assert_eq!(prep.enable_thinking, Some(false));
        assert_eq!(
            prep.chat_template_kwargs,
            Some(json!({"enable_thinking": false}))
        );
        assert_eq!(prep.temperature, Some(0.7));
        assert_eq!(prep.top_p, Some(0.80));
        assert_eq!(prep.presence_penalty, Some(1.5));

        // 4. Tail-injected user message preserves `messages[0]` prefix stability and includes both rubrics
        let tail_msg = prep.build_tail_injected_user_message();
        assert!(tail_msg.starts_with(
            "Convert `foo @skill:refactor` and:\n```\n#steer:think\n```\nto a state machine."
        ));
        assert!(tail_msg.contains("<turn_rubric_and_steering>"));
        assert!(tail_msg.contains("</turn_rubric_and_steering>"));
        assert!(
            tail_msg.contains("Use AST-aware surgical replacements and verify with cargo check.")
        );
        assert!(tail_msg
            .contains("Output only unified diffs or exact replacement blocks. Zero prose."));
    }
}
