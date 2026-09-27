/*
 * SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
 * SPDX-FileCopyrightText: 2026 googlesky
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

package main

import (
	"bamboo-core"
)

// Surrounding text mode, ported from ibus-bamboo's backspace modes without
// their key queue and sleeps: fcitx5 applies deleteSurroundingText and
// commitString before the application gets the key back. The word being
// typed lives in the application, each key replaces its changed tail, and
// cursor movements end the word like in preedit mode.
func (e *FcitxBambooEngine) bsProcessKeyEvent(keyVal uint32, state uint32) bool {
	var keyRune = rune(keyVal)
	if !e.macroEnabled && e.getRawKeyLen() == 0 && !inKeyList(e.preeditor.GetInputMethod().AppendingKeys, keyRune) {
		e.updateLastKeyWithShift(keyVal, state)
		if e.preeditor.CanProcessKey(keyRune) && isValidState(state) {
			if state&FcitxLockMask != 0 {
				keyRune = e.toUpper(keyRune)
			}
			e.preeditor.ProcessKey(keyRune, bamboo.VietnameseMode)
			e.updatePreviousText(e.getPreeditString())
			return true
		}
		return false
	}
	return e.keyPressHandler(keyVal, state)
}

func (e *FcitxBambooEngine) keyPressHandler(keyVal, state uint32) bool {
	defer e.updateLastKeyWithShift(keyVal, state)
	_, oldMacText := e.getMacroText()
	if keyVal == FcitxBackSpace && isValidState(state) {
		if e.getRawKeyLen() == 0 {
			return false
		}
		e.preeditor.RemoveLastChar(true)
		var newText = e.getPreeditString()
		// The application's own backspace is only right when exactly the last
		// character goes away.
		if added, n := e.getOffsetRunes(e.encodeText(newText), e.encodeText(e.bsText)); len(added) == 0 && n == 1 {
			e.bsText = newText
			return false
		}
		e.updatePreviousText(newText)
		return true
	}

	if keyVal == FcitxTab {
		defer e.commitPreeditAndReset("")
		if oldMacText != "" {
			e.updatePreviousText(oldMacText)
			return true
		}
		// Like preedit mode, an invalid word is restored before the tab.
		e.updatePreviousText(e.getComposedString(e.bsText))
		return false
	}

	isValidKey := isValidState(state) && e.isValidKeyVal(keyVal)
	newText, isWordBreakRune := e.getCommitText(keyVal, state)
	e.updatePreviousTextInBatch(newText, isWordBreakRune)
	return isValidKey
}

func (e *FcitxBambooEngine) getPreeditOffset(newRunes, oldRunes []rune) int {
	var minLen = len(oldRunes)
	if len(newRunes) < minLen {
		minLen = len(newRunes)
	}
	for i := 0; i < minLen; i++ {
		if oldRunes[i] != newRunes[i] {
			return i
		}
	}
	return minLen
}

// Diffs against the text the application shows rather than the
// composition: getCommitText may show something else ("f] => f]"). Encoded
// texts are compared, with a legacy charset one letter can be several
// characters.
func (e *FcitxBambooEngine) updatePreviousText(newText string) {
	offsetRunes, nBackSpace := e.getOffsetRunes(e.encodeText(newText), e.encodeText(e.bsText))
	e.SendBackSpace(nBackSpace)
	e.pendingCommit += string(offsetRunes)
	e.bsText = newText
}

func (e *FcitxBambooEngine) updatePreviousTextInBatch(newText string, isWordBreakRune bool) {
	e.updatePreviousText(newText)
	if isWordBreakRune {
		e.commitPreeditAndReset("")
	}
}

// getOffsetRunes returns the right outer text and number of pending backspaces
func (e *FcitxBambooEngine) getOffsetRunes(newText, oldText string) ([]rune, int) {
	var oldRunes = []rune(oldText)
	var newRunes = []rune(newText)
	var nBackSpace = 0
	var offset = e.getPreeditOffset(newRunes, oldRunes)
	if offset < len(oldRunes) {
		nBackSpace += len(oldRunes) - offset
	}

	return newRunes[offset:], nBackSpace
}

// ponytail: the C++ side deletes before it commits, fine with one
// updatePreviousText per key event; fold into pendingCommit if that changes.
func (e *FcitxBambooEngine) SendBackSpace(n int) {
	e.pendingDelete += n
}

func (e *FcitxBambooEngine) takeDeleteCount() int {
	var n = e.pendingDelete
	e.pendingDelete = 0
	return n
}
