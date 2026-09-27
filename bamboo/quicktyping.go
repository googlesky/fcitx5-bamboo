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

// What a w with nothing to mark types.
const (
	standaloneWDefault        = iota // as the input method: ư with Telex W and Telex 2
	standaloneWAlways                // ư with every Telex
	standaloneWNotAtWordStart        // ư, but w at word start like UniKey's option
)

// Rebuilds the preeditor when the w option changes.
func (e *FcitxBambooEngine) setStandaloneW(w int, flags uint) {
	if w == e.standaloneW {
		return
	}
	e.standaloneW = w
	var im = e.inputMethod
	if w != standaloneWDefault {
		im = withStandaloneW(im)
	}
	e.preeditor = bamboo.NewEngine(im, flags)
}

// Adds the lone ư of Telex W to input methods marking the horn with w.
func withStandaloneW(im bamboo.InputMethod) bamboo.InputMethod {
	var horn bool
	for _, rule := range im.Rules {
		if rule.Key == 'w' && rule.EffectType == bamboo.Appending {
			return im
		}
		horn = horn || rule.Key == 'w' && rule.EffectType == bamboo.MarkTransformation && rule.Effect == uint8(bamboo.MarkHorn)
	}
	if !horn {
		return im
	}
	im.Rules = append(append([]bamboo.Rule{}, im.Rules...),
		bamboo.Rule{Key: 'w', EffectType: bamboo.Appending, EffectOn: 'ư', Result: 'ư'})
	im.AppendingKeys = append(append([]rune{}, im.AppendingKeys...), 'w')
	return im
}

// A word starting with w is English with standaloneWNotAtWordStart.
func (e *FcitxBambooEngine) isWordStartW(keyVal, state uint32) bool {
	return e.standaloneW == standaloneWNotAtWordStart && (keyVal == 'w' || keyVal == 'W') &&
		isValidState(state) && e.getRawKeyLen() == 0
}

func (e *FcitxBambooEngine) typeWordStartW(keyVal, state uint32, surrounding bool) bool {
	defer e.updateLastKeyWithShift(keyVal, state)
	e.preeditor.ProcessKey(rune(keyVal), bamboo.EnglishMode)
	if surrounding {
		e.updatePreviousText(e.getPreeditString())
	} else {
		e.updatePreedit(e.getPreeditString())
	}
	return true
}

// OpenKey's quick typing, see quickKeys.
var (
	quickDouble = map[rune]rune{'c': 'h', 'g': 'i', 'k': 'h', 'n': 'g', 'p': 'h', 'q': 'u', 't': 'h'}
	quickStart  = map[rune][2]rune{'f': {'p', 'h'}, 'j': {'g', 'i'}, 'w': {'q', 'u'}}
	quickEnd    = map[rune][2]rune{'g': {'n', 'g'}, 'h': {'n', 'h'}, 'k': {'c', 'h'}}
)

// The word typed with quick typing, when the keys typed are no Vietnamese
// word but their rewrite is: a beginning while typing, a whole word at its
// end. The composition keeps the keys typed for restoring English words.
func (e *FcitxBambooEngine) quickWord(complete bool) (string, bool) {
	if !e.quickDouble && !e.quickStart && !e.quickEnd || e.englishWord || e.preeditor.IsValid(complete) {
		return "", false
	}
	var keys, changed = e.quickKeys([]rune(e.getProcessedString(bamboo.EnglishMode)))
	if !changed {
		return "", false
	}
	var scratch = bamboo.NewEngine(e.preeditor.GetInputMethod(), e.flags)
	for _, key := range keys {
		scratch.ProcessKey(key, bamboo.VietnameseMode)
	}
	if complete && e.spellCheckWithDicts {
		if !e.dictionary[scratch.GetProcessedString(bamboo.VietnameseMode|bamboo.LowerCase)] {
			return "", false
		}
	} else if !scratch.IsValid(complete) {
		return "", false
	}
	return scratch.GetProcessedString(bamboo.VietnameseMode), true
}

// Rewrites cc ch, gg gi, kk kh, nn ng, pp ph, qq qu, tt th; f ph, j gi, w qu
// starting a word; g ng, h nh, k ch ending it, before its tone keys.
func (e *FcitxBambooEngine) quickKeys(keys []rune) ([]rune, bool) {
	var last = len(keys) - 1
	for last >= 0 && inKeyList(e.preeditor.GetInputMethod().ToneKeys, unicode.ToLower(keys[last])) {
		last--
	}
	var out = make([]rune, 0, len(keys)+2)
	for i, key := range keys {
		var lower = unicode.ToLower(key)
		if pair, ok := quickStart[lower]; e.quickStart && ok && i == 0 && len(keys) > 1 {
			out = append(out, withCase(pair[0], key), withCase(pair[1], keys[1]))
		} else if second, ok := quickDouble[lower]; e.quickDouble && ok && len(out) > 0 && unicode.ToLower(out[len(out)-1]) == lower {
			out = append(out, withCase(second, key))
		} else if pair, ok := quickEnd[lower]; e.quickEnd && ok && i == last && i > 0 {
			out = append(out, withCase(pair[0], key), withCase(pair[1], key))
		} else {
			out = append(out, key)
		}
	}
	return out, string(out) != string(keys)
}

func withCase(chr, like rune) rune {
	if unicode.IsUpper(like) {
		return unicode.ToUpper(chr)
	}
	return chr
}
