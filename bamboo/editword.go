/*
 * SPDX-FileCopyrightText: 2026 googlesky
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

package main

import (
	"bamboo-core"
	"unicode"
)

// The longest Vietnamese word, "nghiêng", has 7 letters.
const maxWordLength = 7

// Takes the letters right before the cursor back as the word being typed, so
// that the key edits them like when they were typed: "chao" then "f" gives
// "chào". before is the text before the cursor. Returns the number of
// characters taken back.
func (e *FcitxBambooEngine) editWordBeforeCursor(before string, keyVal, state uint32, surrounding bool) int {
	if e.getRawKeyLen() > 0 || e.outputCharset != "Unicode" || !isValidState(state) ||
		!e.preeditor.CanProcessKey(rune(keyVal)) {
		return 0
	}
	var runes = []rune(before)
	var start = len(runes)
	for start > 0 && unicode.IsLetter(runes[start-1]) {
		start--
	}
	var word, n = string(runes[start:]), len(runes) - start
	// A longer word is no Vietnamese, or starts out of sight.
	if n == 0 || n > maxWordLength || !e.retype(word) {
		return 0
	}
	e.englishWord, e.capitalized = false, false
	if surrounding {
		e.bsText = word
	} else {
		e.SendBackSpace(n)
		e.updatePreedit(word)
	}
	return n
}

// Types word again with the keys of the input method. Refuses words typed
// otherwise, like with the other tone style, and words that are no
// Vietnamese.
func (e *FcitxBambooEngine) retype(word string) bool {
	var keys, ok = wordKeys(e.preeditor.GetInputMethod(), word)
	if !ok {
		return false
	}
	for _, key := range keys {
		e.preeditor.ProcessKey(key, bamboo.VietnameseMode)
	}
	if e.preeditor.GetProcessedString(bamboo.VietnameseMode) != word || !e.preeditor.IsValid(false) {
		e.preeditor.Reset()
		return false
	}
	// Restoring "việt" to its keys would show "vieetj", never typed. Words
	// without marks restore fine: "te" goes on to "text".
	e.madeUpKeys = string(keys) != word
	e.retypedWord, e.retypedKeys = word, string(keys)
	return true
}

// The keys typing word with im: each letter without its mark followed by the
// mark key, the tone key at the end.
func wordKeys(im bamboo.InputMethod, word string) ([]rune, bool) {
	var keys []rune
	var toneKey rune
	for _, chr := range word {
		var lower = unicode.ToLower(chr)
		var marked = bamboo.AddToneToChar(lower, 0)
		var plain = bamboo.AddMarkToTonelessChar(marked, 0)
		if unicode.IsUpper(chr) {
			keys = append(keys, unicode.ToUpper(plain))
		} else {
			keys = append(keys, plain)
		}
		if marked != plain {
			var key = ruleKey(im, func(rule bamboo.Rule) bool {
				return rule.EffectType == bamboo.MarkTransformation && rule.EffectOn == plain && rule.Result == marked
			})
			if key == 0 {
				return nil, false
			}
			keys = append(keys, key)
		}
		if tone := bamboo.FindToneFromChar(lower); tone != bamboo.ToneNone {
			if toneKey != 0 {
				return nil, false
			}
			toneKey = ruleKey(im, func(rule bamboo.Rule) bool {
				return rule.EffectType == bamboo.ToneTransformation && rule.Effect == uint8(tone)
			})
			if toneKey == 0 {
				return nil, false
			}
		}
	}
	if toneKey != 0 {
		keys = append(keys, toneKey)
	}
	return keys, true
}

// The smallest key of the rules matching, 0 if none: rules come in random
// order.
func ruleKey(im bamboo.InputMethod, match func(bamboo.Rule) bool) rune {
	var key rune
	for _, rule := range im.Rules {
		if match(rule) && (key == 0 || rule.Key < key) {
			key = rule.Key
		}
	}
	return key
}
