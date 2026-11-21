//! `spell_test_engine`: loopback HTTP test double for standalone integration tests.
//!
//! Built only with `--features test-support` (into a separate target directory) and never
//! bundled into the app. Serves the `spell_core::test_support::FixtureTransport` engine double
//! over real HTTP + SSE on `127.0.0.1`, prints one JSON line `{"base_url": …, "pid": …}` to
//! stdout once listening, and exits when stdin closes (the parent test process ends).
//!
//! Usage: `spell_test_engine --api-key <key> [--port <port>]`

use std::io::{Read, Write};
use std::sync::Arc;

use spell_core::test_support::{FixtureTransport, LoopbackVerifyServer};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut api_key = String::new();
    let mut port: u16 = 0;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--api-key" if i + 1 < args.len() => {
                api_key = args[i + 1].clone();
                i += 2;
            }
            "--port" if i + 1 < args.len() => {
                port = args[i + 1].parse().unwrap_or_else(|_| {
                    eprintln!("Invalid --port '{}'", args[i + 1]);
                    std::process::exit(2)
                });
                i += 2;
            }
            other => {
                eprintln!(
                    "Unknown argument: {other}\nUsage: spell_test_engine --api-key <key> [--port <port>]"
                );
                std::process::exit(2);
            }
        }
    }
    if api_key.trim().is_empty() {
        eprintln!("--api-key is required");
        std::process::exit(2);
    }
    let server =
        match LoopbackVerifyServer::start_on_port(Arc::new(FixtureTransport::new()), api_key, port)
        {
            Ok(s) => s,
            Err(e) => {
                eprintln!("{e}");
                std::process::exit(1);
            }
        };
    let line = serde_json::json!({ "base_url": server.base_url(), "pid": std::process::id() });
    let mut out = std::io::stdout();
    let _ = writeln!(out, "{line}");
    let _ = out.flush();
    // Serve until the parent closes our stdin.
    let mut sink = [0u8; 64];
    let mut stdin = std::io::stdin();
    while matches!(stdin.read(&mut sink), Ok(n) if n > 0) {}
    drop(server);
}
