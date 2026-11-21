//! Live OpenAI-compatible endpoint contract probes.
//!
//! Ignored by default so `cargo test --offline` stays completely hermetic. Run against any
//! OpenAI-compatible endpoint (OpenAI, Ollama, LM Studio, vLLM, llama.cpp, OpenRouter, etc.):
//!
//! ```bash
//! SPELL_LIVE_ENDPOINT_URL=http://127.0.0.1:11434/v1 \
//!   SPELL_LIVE_MODEL_ID=qwen2.5-coder:7b \
//!   cargo test --offline --test live_engine_contract -- --ignored --nocapture
//! ```

use spell_core::agent_loop::SpellCoreSession;
use spell_core::contract::{ChatCompletionRequest, WireMessage};
use spell_core::locator::EnginePathLocator;
use spell_core::transport::{EngineTransport, HttpTransport};

fn live_transport() -> Option<HttpTransport> {
    let url = std::env::var("SPELL_LIVE_ENDPOINT_URL")
        .ok()
        .filter(|s| !s.trim().is_empty())?;
    Some(HttpTransport::new(url, EnginePathLocator::discover()))
}

fn live_model_id(transport: &HttpTransport) -> String {
    if let Ok(explicit) = std::env::var("SPELL_LIVE_MODEL_ID") {
        if !explicit.trim().is_empty() {
            return explicit.trim().to_string();
        }
    }
    transport
        .list_models()
        .ok()
        .and_then(|models| models.into_iter().next().map(|m| m.id))
        .unwrap_or_else(|| "gpt-4o-mini".to_string())
}

#[test]
#[ignore]
fn live_models_endpoint_lists_available_models() {
    let Some(transport) = live_transport() else {
        return;
    };
    let models = transport
        .list_models()
        .expect("GET /v1/models failed against live endpoint");
    println!("live /models => {} model(s): {:?}", models.len(), models);
    assert!(
        !models.is_empty(),
        "expected at least one model from GET /v1/models"
    );
}

#[test]
#[ignore]
fn live_chat_completion_streams_tokens_and_usage() {
    let Some(transport) = live_transport() else {
        return;
    };
    let model_id = live_model_id(&transport);
    let mut req = ChatCompletionRequest::new(
        model_id,
        vec![
            WireMessage::system("You are a concise assistant."),
            WireMessage::user("Reply with exactly the word READY."),
        ],
    );
    req.max_tokens = Some(16);

    let mut chunk_count = 0usize;
    let summary = transport
        .stream_chat_completion(&mut req, &mut |_chunk| {
            chunk_count += 1;
        })
        .expect("POST /v1/chat/completions failed against live endpoint");

    println!(
        "live stream summary => chunks={chunk_count} content={:?} usage={:?} decode_tok_s={:.1}",
        summary.content, summary.usage, summary.context_advice.decode_tok_s
    );
    assert!(
        !summary.content.trim().is_empty(),
        "expected non-empty assistant response content"
    );
}

#[test]
#[ignore]
fn live_session_configure_and_prompt_roundtrip() {
    let Ok(url) = std::env::var("SPELL_LIVE_ENDPOINT_URL") else {
        return;
    };
    if url.trim().is_empty() {
        return;
    }

    let locator = EnginePathLocator::discover();
    let api_key = locator.resolve_api_key();

    let mut session =
        SpellCoreSession::new_in_memory().expect("SpellCoreSession::new_in_memory");
    session
        .configure_endpoint(Some(&url), Some(api_key.as_deref()), None, None)
        .expect("configure_endpoint failed");
    let (models, latency_ms) = session
        .fetch_models(None, None)
        .expect("fetch_models failed against live endpoint");
    assert!(!models.is_empty(), "expected non-empty models list");
    println!("fetch_models latency={latency_ms}ms count={}", models.len());

    let model_id = std::env::var("SPELL_LIVE_MODEL_ID")
        .ok()
        .filter(|s| !s.trim().is_empty())
        .unwrap_or_else(|| models[0].id.clone());
    session
        .configure_endpoint(None, None, Some(&model_id), None)
        .expect("set model_id failed");

    let ws = std::env::temp_dir().join(format!("spell_live_probe_{}", std::process::id()));
    let _ = std::fs::create_dir_all(&ws);
    session
        .add_project("LiveProbeWorkspace", ws.to_str().unwrap(), false)
        .expect("add_project");
    let tid = session
        .selected_thread_id
        .clone()
        .expect("selected_thread_id");

    let thread = session
        .send_user_prompt(&tid, "Say hello in five words without calling any tools.")
        .expect("send_user_prompt failed");
    assert!(thread.ui_events.iter().any(|e| e.kind == "assistant"));
    let _ = std::fs::remove_dir_all(&ws);
}
