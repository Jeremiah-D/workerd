// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A port of `kj::GlobFilter` (kj/glob-filter.h), the case filter of `workerd bench`, so that
//! case names match in Rust exactly as they do in C++.

/// A glob that matches a name, or the part of it after any `/` or `\`.
///
/// `*` matches any run of characters and `?` any one character, but neither matches `/` or `\`.
/// Names and patterns are bytes, compared one byte at a time, as `kj::GlobFilter` compares
/// `char`s.
#[derive(Debug, Clone)]
pub struct GlobFilter {
    pattern: Vec<u8>,
}

impl GlobFilter {
    #[must_use]
    pub fn new(pattern: &[u8]) -> Self {
        Self {
            pattern: pattern.to_vec(),
        }
    }

    /// Whether `name` matches. Runs a nondeterministic finite automaton with one state per byte
    /// of the pattern, keeping the list of states it is in.
    #[must_use]
    pub fn matches(&self, name: &[u8]) -> bool {
        let mut states = vec![0];
        let mut next = Vec::new();
        for &c in name {
            next.clear();
            // The pattern can omit a leading path, so after a separator, matching starts again.
            if is_separator(c) {
                next.push(0);
            }
            for &state in &states {
                self.apply_state(c, state, &mut next);
            }
            std::mem::swap(&mut states, &mut next);
        }
        // A state at the end of the pattern, or at wildcards just before it, is a match.
        states.iter().any(|&state| {
            self.pattern
                .get(state..)
                .is_some_and(|rest| rest.iter().all(|&p| p == b'*'))
        })
    }

    /// Adds to `next` the states that `c` takes `state` to.
    fn apply_state(&self, c: u8, state: usize, next: &mut Vec<usize>) {
        match self.pattern.get(state) {
            Some(b'*') => {
                // A '*' both stays and tries to match the next state.
                if !is_separator(c) {
                    next.push(state);
                }
                self.apply_state(c, state + 1, next);
            }
            Some(b'?') => {
                if !is_separator(c) {
                    next.push(state + 1);
                }
            }
            Some(&p) if c == p => next.push(state + 1),
            Some(_) | None => {}
        }
    }
}

fn is_separator(c: u8) -> bool {
    c == b'/' || c == b'\\'
}

#[cfg(test)]
#[path = "glob_filter-test.rs"]
mod tests;
