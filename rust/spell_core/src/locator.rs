//! Dynamic path & credential discovery (`EnginePathLocator`) for BeatrixLeslie IDE (`spell_core`).
//!
//! Resolves active workspace roots, application data and log directories, and optional
//! API keys from standard environment variables without any hardcoded user paths
//! or proprietary external config files.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

use directories::BaseDirs;

/// Dynamic locator for workspace directories, application data, logs, and optional
/// environment-provided API credentials.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EnginePathLocator {
    pub engine_root: Option<PathBuf>,
    pub workspace_roots: Vec<PathBuf>,
    pub data_dir: PathBuf,
    pub logs_dir: PathBuf,
}

impl Default for EnginePathLocator {
    fn default() -> Self {
        Self::discover()
    }
}

impl EnginePathLocator {
    /// Discovers workspace roots, application data directory, and logs directory dynamically
    /// from environment variables, process working directory, and platform standard directories.
    pub fn new() -> Self {
        Self::discover()
    }

    /// Discovers `workspace_roots`, `data_dir`, and `logs_dir`.
    pub fn discover() -> Self {
        let data_dir = Self::resolve_data_dir();
        let logs_dir = Self::resolve_logs_dir(&data_dir);
        Self {
            engine_root: Self::resolve_engine_root(),
            workspace_roots: Self::resolve_workspace_roots(),
            data_dir,
            logs_dir,
        }
    }

    /// BeatrixLeslie IDE is endpoint-agnostic and does not discover any external engine source tree.
    pub fn resolve_engine_root() -> Option<PathBuf> {
        None
    }

    /// Resolves `workspace_roots` (used only when no project is selected):
    /// 1. `SPELL_WORKSPACE_ROOT` environment variable (OS path-list separated via `env::split_paths`)
    /// 2. Current working directory, unless it is `/` (GUI apps start there)
    /// 3. `<data_dir>/Workspace`, a folder owned by the app
    pub fn resolve_workspace_roots() -> Vec<PathBuf> {
        let mut roots = Vec::new();

        // 1. `SPELL_WORKSPACE_ROOT` (cross-platform `env::split_paths`)
        if let Ok(val) = env::var("SPELL_WORKSPACE_ROOT") {
            for part in env::split_paths(&val) {
                let trimmed = part.to_string_lossy();
                let trimmed = trimmed.trim();
                if !trimmed.is_empty() {
                    Self::push_unique_path(&mut roots, PathBuf::from(trimmed));
                }
            }
        }

        // 2. Current working directory (command-line use), never the filesystem root
        if let Ok(cwd) = env::current_dir() {
            if cwd.parent().is_some() {
                Self::push_unique_path(&mut roots, cwd);
            }
        }

        // 3. The app's own workspace folder
        if roots.is_empty() {
            let own = Self::resolve_data_dir().join("Workspace");
            let _ = fs::create_dir_all(&own);
            Self::push_unique_path(&mut roots, own);
        }

        roots
    }

    /// Resolves `data_dir`:
    /// 1. `SPELL_DATA_DIR` env var
    /// 2. Platform local data dir (`directories::BaseDirs::data_local_dir().join("SpellIDE")`):
    ///    - macOS: `~/Library/Application Support/SpellIDE`
    ///    - Linux: `$XDG_DATA_HOME/SpellIDE` or `~/.local/share/SpellIDE`
    ///    - Windows: `%LOCALAPPDATA%\SpellIDE`
    /// 3. Fallback: `std::env::temp_dir().join("SpellIDE")`
    pub fn resolve_data_dir() -> PathBuf {
        let env_override = env::var("SPELL_DATA_DIR").ok();
        Self::resolve_data_dir_with_override(env_override.as_deref())
    }

    fn resolve_data_dir_with_override(env_override: Option<&str>) -> PathBuf {
        if let Some(raw) = env_override {
            let trimmed = raw.trim();
            if !trimmed.is_empty() {
                return PathBuf::from(trimmed);
            }
        }

        if let Some(base) = BaseDirs::new() {
            return base.data_local_dir().join("SpellIDE");
        }

        env::temp_dir().join("SpellIDE")
    }

    /// Resolves `logs_dir`:
    /// 1. `SPELL_LOGS_DIR` env var
    /// 2. `<data_dir>/logs`
    pub fn resolve_logs_dir(data_dir: &Path) -> PathBuf {
        let env_override = env::var("SPELL_LOGS_DIR").ok();
        Self::resolve_logs_dir_with_override(data_dir, env_override.as_deref())
    }

    fn resolve_logs_dir_with_override(data_dir: &Path, env_override: Option<&str>) -> PathBuf {
        if let Some(raw) = env_override {
            let trimmed = raw.trim();
            if !trimmed.is_empty() {
                return PathBuf::from(trimmed);
            }
        }

        data_dir.join("logs")
    }

    /// Resolves an optional API key from environment variables (`SPELL_API_KEY`, then `OPENAI_API_KEY`).
    ///
    /// Returns `None` when unset (requests are then sent without an `Authorization` header;
    /// the app normally supplies keys via `configure_endpoint`). Never fabricates a key.
    pub fn resolve_api_key(&self) -> Option<String> {
        self.resolve_configured_api_key()
    }

    /// Resolves a configured API key strictly from `["SPELL_API_KEY", "OPENAI_API_KEY"]`.
    pub fn resolve_configured_api_key(&self) -> Option<String> {
        for env_key in ["SPELL_API_KEY", "OPENAI_API_KEY"] {
            if let Ok(val) = env::var(env_key) {
                let trimmed = val.trim();
                if !trimmed.is_empty() {
                    return Some(trimmed.to_string());
                }
            }
        }
        None
    }

    /// Builds standard OpenAI HTTP bearer authentication header (`Authorization: Bearer <key>`)
    /// when a non-empty API key is configured; returns an empty `Vec` when unset.
    pub fn build_auth_headers(&self) -> Vec<(String, String)> {
        let Some(key) = self.resolve_api_key() else {
            return Vec::new();
        };
        if key.is_empty() {
            return Vec::new();
        }
        vec![("Authorization".to_string(), format!("Bearer {key}"))]
    }

    /// Returns the primary workspace root, falling back to `.` if none are available.
    pub fn primary_workspace_root(&self) -> PathBuf {
        self.workspace_roots
            .first()
            .cloned()
            .unwrap_or_else(|| PathBuf::from("."))
    }

    fn push_unique_path(list: &mut Vec<PathBuf>, candidate: PathBuf) {
        if !list.iter().any(|existing| existing == &candidate) {
            list.push(candidate);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_locator_discovers_without_hardcoded_paths() {
        let locator = EnginePathLocator::discover();
        assert!(locator.engine_root.is_none());
        assert!(!locator.workspace_roots.is_empty());
        assert!(!locator.data_dir.as_os_str().is_empty());
        assert!(!locator.logs_dir.as_os_str().is_empty());
        assert_eq!(
            locator.logs_dir,
            EnginePathLocator::resolve_logs_dir(&locator.data_dir)
        );

        let headers = locator.build_auth_headers();
        if locator.resolve_api_key().is_some() {
            assert_eq!(headers.len(), 1);
            assert_eq!(headers[0].0, "Authorization");
            assert!(headers[0].1.starts_with("Bearer "));
        } else {
            assert!(
                headers.is_empty(),
                "no configured key -> no auth headers (never a made-up key)"
            );
        }
    }

    #[test]
    fn test_resolve_data_dir_override_and_platform_default() {
        let custom = EnginePathLocator::resolve_data_dir_with_override(Some("  /custom/spell_data  "));
        assert_eq!(custom, PathBuf::from("/custom/spell_data"));

        let default_dir = EnginePathLocator::resolve_data_dir_with_override(Some("   "));
        assert_eq!(
            default_dir.file_name().and_then(|n| n.to_str()),
            Some("SpellIDE")
        );
        if let Some(base) = BaseDirs::new() {
            assert_eq!(default_dir, base.data_local_dir().join("SpellIDE"));
        } else {
            assert_eq!(default_dir, env::temp_dir().join("SpellIDE"));
        }
    }

    #[test]
    fn test_resolve_logs_dir_override_and_default() {
        let data_dir = PathBuf::from("/example/SpellIDE");
        assert_eq!(
            EnginePathLocator::resolve_logs_dir_with_override(&data_dir, None),
            data_dir.join("logs")
        );
        assert_eq!(
            EnginePathLocator::resolve_logs_dir_with_override(&data_dir, Some("   ")),
            data_dir.join("logs")
        );
        assert_eq!(
            EnginePathLocator::resolve_logs_dir_with_override(&data_dir, Some(" /var/spell_logs ")),
            PathBuf::from("/var/spell_logs")
        );
    }

    #[test]
    fn test_split_paths_portable_workspace_roots() {
        let p1 = env::temp_dir().join("spell_ws_alpha");
        let p2 = env::temp_dir().join("spell_ws_beta");
        let joined = env::join_paths([&p1, &p2]).unwrap();
        let parsed: Vec<PathBuf> = env::split_paths(&joined)
            .map(|p| PathBuf::from(p.to_string_lossy().trim()))
            .filter(|p| !p.as_os_str().is_empty())
            .collect();
        assert_eq!(parsed, vec![p1, p2]);
    }
}
