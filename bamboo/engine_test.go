/*
 * SPDX-FileCopyrightText: 2026 googlesky
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

package main

import (
	"bamboo-core"
	"math/rand"
	"sort"
	"testing"
)

const (
	keyReturn   = 0xff0d
	keyControlL = 0xffe3
	keyLeft     = 0xff51
)

// testApp plays the C++ side plus the application: deletions, commits and
// the keys the engine lets through end up in text. Preedit mode keeps the
// word in e.preeditText, surrounding text mode edits text directly.
type testApp struct {
	e           *FcitxBambooEngine
	surrounding bool
	editWord    bool // like the C++ side with EditWordBeforeCursor
	text        []rune
}

func newTestApp(imName string, macros [][2]string, surrounding bool) *testApp {
	var im = bamboo.ParseInputMethod(bamboo.InputMethodDefinitions, imName)
	var e = newFcitxBambooEngine(im, map[string]bool{}, newMacroTable(macros))
	e.macroEnabled = macros != nil
	e.autoCapitalizeMacro = true
	return &testApp{e: e, surrounding: surrounding}
}

func (a *testApp) press(keyVal, state uint32) bool {
	if a.editWord {
		var before = a.text
		if len(before) > maxWordLength+1 {
			before = before[len(before)-maxWordLength-1:]
		}
		a.e.editWordBeforeCursor(string(before), keyVal, state, a.surrounding)
	}
	var handled = a.e.processKeyEvent(keyVal, state, a.surrounding)
	var n = a.e.takeDeleteCount()
	if n > len(a.text) {
		panic("deleting more than the text")
	}
	a.text = append(a.text[:len(a.text)-n], []rune(a.e.takeCommitText())...)
	if !handled && keyVal == FcitxBackSpace && state&FcitxControlMask != 0 {
		for len(a.text) > 0 && a.text[len(a.text)-1] != ' ' {
			a.text = a.text[:len(a.text)-1]
		}
	}
	if !handled && state&(FcitxControlMask|FcitxMod1Mask|FcitxMod4Mask) == 0 {
		switch {
		case keyVal == FcitxBackSpace && len(a.text) > 0:
			a.text = a.text[:len(a.text)-1]
		case keyVal == keyReturn:
			a.text = append(a.text, '\n')
		case keyVal == FcitxTab:
			a.text = append(a.text, '\t')
		case keyVal >= 0x20 && keyVal < 0x7f:
			a.text = append(a.text, rune(keyVal))
		}
	}
	return handled
}

// typeString types ASCII keys, \b \t \n \x01 being BackSpace, Tab, Return
// and Left.
func (a *testApp) typeString(s string) {
	for _, c := range s {
		var keyVal = map[rune]uint32{'\b': FcitxBackSpace, '\t': FcitxTab, '\n': keyReturn, '\x01': keyLeft}[c]
		if keyVal == 0 {
			keyVal = uint32(c)
		}
		a.press(keyVal, 0)
	}
}

// Cases ported from ibus-bamboo engine_test.go (TestPreeditEngine).
func TestPreeditEngine(t *testing.T) {
	var vn = [][2]string{{"vn", "việt nam"}}
	var arrow = [][2]string{{"->", "arrow"}}
	var star = [][2]string{{"csao", "✪"}, {"csao2", "✬"}}
	for _, tc := range []struct {
		name     string
		macros   [][2]string
		keys     string
		preedits []string    // expected preedit after each key of keys
		after    [][2]uint32 // key events sent after keys
		text     string      // application text at the end
	}{
		{name: "control_a", after: [][2]uint32{{keyControlL, 0}, {'a', FcitxControlMask}}},
		{name: "macro_control_a", macros: arrow, after: [][2]uint32{{keyControlL, 0}, {'a', FcitxControlMask}}},
		{name: "duowidro", keys: "duowidro", preedits: []string{"d", "du", "duo", "dươ", "dươi", "đươi", "đưởi", "đuổi"}},
		{name: "duowidro_enter", keys: "duowidro", after: [][2]uint32{{keyReturn, 0}}, text: "đuổi\n"},
		{name: "macro_vowl_space", macros: vn, keys: "vowl ", preedits: []string{"v", "vo", "vơ", "vơl", ""},
			after: [][2]uint32{{keyControlL, 0}}, text: "vowl "},
		{name: "macro_vowl_enter", macros: vn, keys: "vowl", after: [][2]uint32{{keyReturn, 0}}, text: "vowl\n"},
		{name: "macro_duowidro_enter", macros: vn, keys: "duowidro", after: [][2]uint32{{keyReturn, 0}}, text: "đuổi\n"},
		{name: "workaround_spreadsheet_number_enter", keys: "12", preedits: []string{"", ""},
			after: [][2]uint32{{keyReturn, 0}}, text: "12\n"},
		{name: "macro_vn_dot", macros: vn, keys: "vn.", preedits: []string{"v", "vn", ""}, text: "việt nam."},
		{name: "macro_vn_comma_space", macros: vn, keys: "vn, ", after: [][2]uint32{{keyReturn, 0}}, text: "việt nam, \n"},
		{name: "macro_vn_enter", macros: vn, keys: "vn", after: [][2]uint32{{keyReturn, 0}}, text: "việt nam\n"},
		{name: "macro_arrow_dot", macros: arrow, keys: "->.", preedits: []string{"-", "->", ""}, text: "arrow."},
		{name: "macro_arrow_enter", macros: arrow, keys: "->", after: [][2]uint32{{keyReturn, 0}}, text: "arrow\n"},
		{name: "macro_csao_space", macros: star, keys: "csao ", text: "✪ "},
		{name: "macro_csao2_enter", macros: star, keys: "csao2", after: [][2]uint32{{keyReturn, 0}}, text: "✬\n"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			a := newTestApp("Telex", tc.macros, false)
			for i, c := range tc.keys {
				a.press(uint32(c), 0)
				if i < len(tc.preedits) && a.e.preeditText != tc.preedits[i] {
					t.Errorf("after %q: preedit = %q, want %q", tc.keys[:i+1], a.e.preeditText, tc.preedits[i])
				}
			}
			for _, k := range tc.after {
				a.press(k[0], k[1])
			}
			if string(a.text) != tc.text {
				t.Errorf("text = %q, want %q", string(a.text), tc.text)
			}
		})
	}
}

func TestSuperShortcutIsNotTyped(t *testing.T) {
	a := newTestApp("Telex", nil, false)
	if a.press('a', FcitxMod4Mask) || a.e.preeditText != "" || len(a.text) != 0 {
		t.Errorf("Super+a was typed: preedit %q, text %q", a.e.preeditText, string(a.text))
	}
}

// Macro keys follow ibus-bamboo: case-insensitive only with auto capitalize.
func TestMacroCase(t *testing.T) {
	for _, tc := range []struct {
		name       string
		macros     [][2]string
		autoCap    bool
		keys, text string
	}{
		{"upper_key_lower_typed", [][2]string{{"VN", "Việt Nam"}}, true, "vn ", "việt nam "},
		{"lower_key_upper_typed", [][2]string{{"vn", "Việt Nam"}}, true, "VN ", "VIỆT NAM "},
		{"no_autocap_exact_only", [][2]string{{"vn", "Việt Nam"}}, false, "VN ", "VN "},
		{"no_autocap_exact", [][2]string{{"vn", "Việt Nam"}}, false, "vn ", "Việt Nam "},
		{"prefix_ignores_case", [][2]string{{"a1", "xyz"}}, true, "A1 ", "xyz "},
		{"empty_key_ignored", [][2]string{{"", "oops"}, {" ", "oops"}}, true, "a ", "a "},
		{"empty_value_ignored", [][2]string{{"vn", ""}}, true, "vn ", "vn "},
		{"exact_key_first", [][2]string{{"ms", "Microsoft"}, {"MS", "Mississippi"}}, true, "ms MS ", "microsoft MISSISSIPPI "},
	} {
		t.Run(tc.name, func(t *testing.T) {
			a := newTestApp("Telex", tc.macros, false)
			a.e.autoCapitalizeMacro = tc.autoCap
			a.typeString(tc.keys)
			if string(a.text) != tc.text {
				t.Errorf("text = %q, want %q", string(a.text), tc.text)
			}
		})
	}
}

func TestCommitUsesOutputCharset(t *testing.T) {
	a := newTestApp("Telex", nil, false)
	a.e.outputCharset = "TCVN3 (ABC)"
	a.typeString("tieengs vieetj\n")
	if want := bamboo.Encode("TCVN3 (ABC)", "tiếng việt") + "\n"; string(a.text) != want {
		t.Errorf("text = %q, want %q", string(a.text), want)
	}
}

// A Go panic inside a cgo call would kill fcitx5.
func TestExportsSurviveInvalidHandle(t *testing.T) {
	if EngineProcessKeyEvent(0xdead, 'a', 0, false) {
		t.Error("invalid engine handled a key")
	}
	ResetEngine(0xdead)
	DeleteObject(0xdead)
}

func TestSurroundingEngine(t *testing.T) {
	for _, tc := range []struct {
		name, keys, text string
		macros           [][2]string
		charset          string
		noSpellCheck     bool
	}{
		{name: "words", keys: "tieengs vieetj\n", text: "tiếng việt\n"},
		{name: "backspace_last_char", keys: "tieengs\bg", text: "tiếng"},
		{name: "backspace_moves_tone", keys: "hoafn\b", text: "hòa"},
		{name: "backspace_moves_tone_no_spell_check", keys: "hoafn\b", text: "hòa", noSpellCheck: true},
		{name: "backspace_whole_word", keys: "dd\b\bx", text: "x"},
		{name: "restore_invalid_word", keys: "text ", text: "text "},
		{name: "undo_mark", keys: "aaa ", text: "aa "},
		{name: "number", keys: "12", text: "12"},
		{name: "macro", keys: "vn, ", text: "việt nam, ", macros: [][2]string{{"vn", "việt nam"}}},
		{name: "charset", keys: "tieengs", text: bamboo.Encode("TCVN3 (ABC)", "tiếng"), charset: "TCVN3 (ABC)"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			a := newTestApp("Telex", tc.macros, true)
			if tc.charset != "" {
				a.e.outputCharset = tc.charset
			}
			a.e.autoNonVnRestore = !tc.noSpellCheck
			a.typeString(tc.keys)
			if string(a.text) != tc.text {
				t.Errorf("text = %q, want %q", string(a.text), tc.text)
			}
			if a.e.preeditText != "" {
				t.Errorf("preedit = %q in surrounding text mode", a.e.preeditText)
			}
		})
	}
}

// Proper nouns and jargon the spell check would restore to keys.
func TestSpellCheckExceptions(t *testing.T) {
	for _, surrounding := range []bool{false, true} {
		a := newTestApp("Telex", nil, surrounding)
		a.typeString("Kroong ")
		if string(a.text) != "Kroong " {
			t.Errorf("surrounding %v: without exception %q", surrounding, string(a.text))
		}
		a = newTestApp("Telex", nil, surrounding)
		a.e.spellCheckExceptions = []string{"krông"}
		a.typeString("Kroong KROONG Kroongs text ")
		if string(a.text) != "Krông KRÔNG Kroongs text " {
			t.Errorf("surrounding %v: with exception %q", surrounding, string(a.text))
		}
	}
}

// Ctrl+BackSpace deletes a whole word in the application, it must end the
// word like any other shortcut.
func TestControlBackSpaceEndsWord(t *testing.T) {
	for _, surrounding := range []bool{false, true} {
		a := newTestApp("Telex", nil, surrounding)
		a.typeString("abc tien")
		if a.press(FcitxBackSpace, FcitxControlMask) {
			t.Errorf("surrounding %v: Ctrl+BackSpace did not reach the application", surrounding)
		}
		a.typeString("e")
		if string(a.text)+a.e.preeditText != "abc e" {
			t.Errorf("surrounding %v: text %q preedit %q", surrounding, string(a.text), a.e.preeditText)
		}
	}
}

// Surrounding text mode must show exactly what preedit mode shows (commits
// plus preedit) after every key, whatever the input method and options.
func TestModesAgree(t *testing.T) {
	var keys = []rune("aeiouydwsfrxjztnhgcmqplkvbAEOUDWS  ,.'`~^+([]{}0123456789\b\b\b\t\n\x01")
	var ims []string
	for name := range bamboo.InputMethodDefinitions {
		ims = append(ims, name)
	}
	sort.Strings(ims)
	var dict = map[string]bool{"việt": true, "tiếng": true, "tôi": true}
	var options = []struct {
		restore, dict, modern, editWord bool
		charset                         string
	}{
		{true, false, false, false, "Unicode"},
		{false, false, false, false, "Unicode"},
		{true, true, true, false, "Unicode"},
		{true, false, false, false, "TCVN3 (ABC)"},
		{true, false, true, false, "VIQR"},
		{true, false, false, true, "Unicode"},
		{false, false, true, true, "Unicode"},
	}
	for _, im := range ims {
		for _, o := range options {
			for _, macros := range [][][2]string{nil, {{"vn", "việt nam"}, {"->", "→"}}} {
				for seed := int64(0); seed < 40; seed++ {
					var r = rand.New(rand.NewSource(seed))
					var p, b = newTestApp(im, macros, false), newTestApp(im, macros, true)
					p.editWord, b.editWord = o.editWord, o.editWord
					for _, e := range []*FcitxBambooEngine{p.e, b.e} {
						e.autoNonVnRestore, e.spellCheckWithDicts, e.dictionary = o.restore, o.dict, dict
						e.outputCharset = o.charset
						if o.modern {
							e.preeditor.SetFlag(bamboo.EstdFlags &^ bamboo.EstdToneStyle)
						}
					}
					var typed string
					for i := 0; i < 14; i++ {
						var key = string(keys[r.Intn(len(keys))])
						typed += key
						p.typeString(key)
						b.typeString(key)
						if string(p.text)+p.e.preeditText != string(b.text) {
							t.Fatalf("%s %+v macros %v keys %q: preedit mode %q+%q, surrounding %q",
								im, o, macros != nil, typed, string(p.text), p.e.preeditText, string(b.text))
						}
					}
				}
			}
		}
	}
}

// A key right after a word edits it like one being typed.
func TestEditWordBeforeCursor(t *testing.T) {
	for _, tc := range []struct {
		name, im, text, keys, want string
	}{
		{name: "tone", text: "xin chao", keys: "f", want: "xin chào"},
		{name: "after_backspace", keys: "vieet \bj ", want: "việt "},
		{name: "tone_moves", text: "hòa", keys: "s", want: "hóa"},
		{name: "mark", text: "tuong", keys: "w", want: "tương"},
		{name: "upper_case", text: "VIêt", keys: "j", want: "VIệt"},
		{name: "after_punctuation", text: "(viet", keys: "j", want: "(viẹt"},
		{name: "vni", im: "VNI", text: "viet", keys: "65", want: "việt"},
		{name: "other_tone_style", text: "hoà", keys: "s", want: "hoàs"},
		{name: "not_vietnamese", text: "hello", keys: "s", want: "hellos"},
		{name: "longer_than_a_word", text: "xnghieng", keys: "s", want: "xnghiengs"},
		{name: "not_a_typing_key", text: "viet", keys: "1", want: "viet1"},
		{name: "made_up_keys_never_restored", text: "việt", keys: "f ", want: "việtf "},
		{name: "next_word_restored", text: "việt", keys: "f class ", want: "việtf class "},
		{name: "english_word_goes_on", text: "te", keys: "xt ", want: "text "},
	} {
		for _, surrounding := range []bool{false, true} {
			var im = tc.im
			if im == "" {
				im = "Telex"
			}
			a := newTestApp(im, nil, surrounding)
			a.editWord = true
			a.text = []rune(tc.text)
			a.typeString(tc.keys)
			if got := string(a.text) + a.e.preeditText; got != tc.want {
				t.Errorf("%s surrounding %v: got %q, want %q", tc.name, surrounding, got, tc.want)
			}
		}
	}
}

// Preedit mode moves the word into the preedit, surrounding text mode
// replaces its changed tail.
func TestEditWordBeforeCursorOutput(t *testing.T) {
	a := newTestApp("Telex", nil, false)
	if n := a.e.editWordBeforeCursor("xin toi", 's', 0, false); n != 3 ||
		a.e.takeDeleteCount() != 3 || a.e.preeditText != "toi" {
		t.Errorf("preedit mode: took %d, preedit %q", n, a.e.preeditText)
	}
	a = newTestApp("Telex", nil, true)
	a.e.editWordBeforeCursor("xin toi", 's', 0, true)
	a.e.processKeyEvent('s', 0, true)
	if n, commit := a.e.takeDeleteCount(), a.e.takeCommitText(); n != 2 || commit != "ói" {
		t.Errorf("surrounding mode: deleted %d, committed %q", n, commit)
	}
	a = newTestApp("Telex", nil, false)
	a.typeString("vie")
	if a.e.editWordBeforeCursor("toi", 's', 0, false) != 0 {
		t.Error("took a word while composing")
	}
	a = newTestApp("Telex", nil, false)
	a.e.outputCharset = "TCVN3 (ABC)"
	if a.e.editWordBeforeCursor("toi", 's', 0, false) != 0 {
		t.Error("took a word with a legacy charset")
	}
	a = newTestApp("Telex", nil, false)
	if a.e.editWordBeforeCursor("toi", 's', FcitxControlMask, false) != 0 {
		t.Error("took a word for a shortcut")
	}
}

// UniKey and ibus-bamboo macro files.
func TestParseMacroText(t *testing.T) {
	var text = "\uFEFF;DO NOT DELETE THIS LINE*** version=1 ***\r\n" +
		"# DO NOT DELETE THIS LINE*** version=1 ***\n" +
		"#vn:commented\n; also a comment\n\n" +
		"vn:Việt Nam\r\nhcm:HCM\n hcm : Hồ Chí Minh \nurl:http://example.com\nno colon\n:no key\nempty:\n"
	var got = parseMacroText(text)
	var want = [][2]string{{"vn", "Việt Nam"}, {"hcm", "Hồ Chí Minh"}, {"url", "http://example.com"}}
	if len(got) != len(want) {
		t.Fatalf("got %q, want %q", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("entry %d: got %q, want %q", i, got[i], want[i])
		}
	}
}
