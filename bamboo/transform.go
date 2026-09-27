/*
 * SPDX-FileCopyrightText: 2026 googlesky
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

package main

import (
	"bamboo-core"
	"sort"
	"strings"
	"unicode"
)

// UniKey toolkit's conversions of a selection: kind then result for each
// one changing text. Kinds are retype, plain, upper, lower, title, and the
// legacy charsets text may be written in.
func (e *FcitxBambooEngine) textTransforms(text string) []string {
	var out []string
	var add = func(kind, result string) {
		if result != text {
			out = append(out, kind, result)
		}
	}
	add("retype", e.retypeText(text))
	add("plain", removeDiacritics(text))
	add("upper", strings.ToUpper(text))
	add("lower", strings.ToLower(text))
	add("title", titleCase(text))
	var charsets = bamboo.GetCharsetNames()
	sort.Strings(charsets)
	for _, charset := range charsets {
		if charset != bamboo.UNICODE {
			add(charset, decodeCharset(charset, text))
		}
	}
	return out
}

// Text typed with the input method off, typed again: "vieejt" gives
// "việt". Words making no Vietnamese stay.
func (e *FcitxBambooEngine) retypeText(text string) string {
	var out, word []rune
	var flush = func() {
		var scratch = bamboo.NewEngine(e.preeditor.GetInputMethod(), e.flags)
		for _, key := range word {
			scratch.ProcessKey(key, bamboo.VietnameseMode)
		}
		if len(word) > 0 && scratch.IsValid(true) {
			out = append(out, []rune(scratch.GetProcessedString(bamboo.VietnameseMode))...)
		} else {
			out = append(out, word...)
		}
		word = word[:0]
	}
	for _, chr := range text {
		if e.preeditor.CanProcessKey(chr) {
			word = append(word, chr)
			continue
		}
		flush()
		out = append(out, chr)
	}
	flush()
	return string(out)
}

// Every word starts with an upper case letter, the rest is lower case.
func titleCase(text string) string {
	var runes = []rune(text)
	var start = true
	for i, r := range runes {
		if unicode.IsLetter(r) {
			if start {
				runes[i] = unicode.ToUpper(r)
			} else {
				runes[i] = unicode.ToLower(r)
			}
			start = false
		} else {
			start = r != '\'' && r != '’'
		}
	}
	return string(runes)
}

// Legacy charset text back to Unicode, longest encoded letters first. TCVN3
// shares codes between cases (upper case came with its own fonts), lower
// case is taken then.
func decodeCharset(charset, text string) string {
	var letters = map[string]rune{}
	var longest = 0
	for _, lower := range append([]rune("đ"), bamboo.Vowels...) {
		for _, letter := range []rune{lower, unicode.ToUpper(lower)} {
			if encoded := bamboo.Encode(charset, string(letter)); encoded != string(letter) && letters[encoded] == 0 {
				letters[encoded] = letter
				if n := len([]rune(encoded)); n > longest {
					longest = n
				}
			}
		}
	}
	var runes = []rune(text)
	var out []rune
	for i := 0; i < len(runes); {
		var n = longest
		if n > len(runes)-i {
			n = len(runes) - i
		}
		for ; n > 0; n-- {
			if letter, ok := letters[string(runes[i:i+n])]; ok {
				out = append(out, letter)
				break
			}
		}
		if n == 0 {
			out = append(out, runes[i])
			n = 1
		}
		i += n
	}
	return string(out)
}
