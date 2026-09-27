/*
 * SPDX-FileCopyrightText: 2026 googlesky
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

package main

import (
	"bamboo-core"
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
