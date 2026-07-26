'use strict';
/*
 * chat.js -- 1.8 chat components down to something a 480p GameCube can draw.
 *
 * S02 carries a JSON component tree with nesting, translation keys, hover and
 * click events and per-span formatting. The console has a 95-glyph ASCII font
 * (chars 32..126, source/font_gen.h) and one colour byte per line, so this
 * flattens the tree to text, folds anything outside that range, and reduces the
 * formatting to the line's first colour.
 *
 * S02 with position byte 2 is the action bar, not chat -- that is where
 * MegaSkywars puts the border radius and everything else the player must not
 * miss, and the console draws it always-on while the chat log is hidden by
 * default. Getting that byte wrong sends the one line that matters into a log
 * nobody has open.
 */

const SECTION = '§';
const COLOUR_CODES = '0123456789abcdef';

/**
 * Vanilla translation keys the server can send that are not the plugin's own
 * text. The plugin builds its messages with ChatColor and plain strings, so
 * this only has to cover what Spigot itself emits; an unknown key falls back
 * to its arguments, which reads as "PlayerName" rather than as nothing.
 */
const TRANSLATIONS = {
    'chat.type.text': '<%s> %s',
    'chat.type.announcement': '[%s] %s',
    'chat.type.emote': '* %s %s',
    'chat.type.admin': '[%s: %s]',
    'multiplayer.player.joined': '%s joined the game',
    'multiplayer.player.left': '%s left the game',
    'commands.message.display.incoming': '%s whispers to you: %s',
    'commands.message.display.outgoing': 'You whisper to %s: %s',
    'death.attack.player': '%s was slain by %s',
    'death.attack.arrow': '%s was shot by %s',
    'death.attack.fall': '%s fell from a high place',
    'death.attack.outOfWorld': '%s fell out of the world',
    'death.attack.generic': '%s died',
};

/** MC colour name -> code, for components that use "color" instead of a code. */
const COLOUR_NAMES = {
    black: '0', dark_blue: '1', dark_green: '2', dark_aqua: '3',
    dark_red: '4', dark_purple: '5', gold: '6', gray: '7',
    dark_gray: '8', blue: '9', green: 'a', aqua: 'b',
    red: 'c', light_purple: 'd', yellow: 'e', white: 'f',
};

/**
 * Flatten a component to a string that still carries section-sign codes, so
 * the colour survives to be extracted a step later. `parent` inherits colour
 * down the tree the way the vanilla renderer does.
 */
function toText(component, parentColour = null) {
    if (component === null || component === undefined) return '';
    if (typeof component === 'string') return component;
    if (Array.isArray(component)) {
        return component.map((c) => toText(c, parentColour)).join('');
    }
    if (typeof component !== 'object') return String(component);

    const colour = COLOUR_NAMES[component.color] || parentColour;
    let out = colour && colour !== parentColour ? SECTION + colour : '';

    if (typeof component.text === 'string') {
        out += component.text;
    } else if (component.translate) {
        const args = (component.with || []).map((c) => toText(c, colour));
        out += format(TRANSLATIONS[component.translate], component.translate, args);
    } else if (component.score && component.score.value !== undefined) {
        out += String(component.score.value);
    } else if (component.selector) {
        out += String(component.selector);
    }

    for (const extra of component.extra || []) out += toText(extra, colour);
    return out;
}

/** Minecraft's %s / %1$s substitution. An unknown key degrades to its own
 *  arguments joined by spaces, which is nearly always the player names. */
function format(pattern, key, args) {
    if (!pattern) return args.length ? args.join(' ') : key;
    let i = 0;
    return pattern.replace(/%(?:(\d+)\$)?s/g, (_, n) =>
        n ? (args[n - 1] ?? '') : (args[i++] ?? ''));
}

/**
 * Fold to the console's printable range (32..126), preserving section-sign
 * codes. Accented Latin and the box-drawing characters the plugin uses as
 * separators become their nearest ASCII, and anything left over is dropped
 * rather than drawn as a missing glyph.
 */
const FOLD = {
    ' ': ' ', '‘': "'", '’': "'", '“': '"', '”': '"',
    '–': '-', '—': '-', '•': '*', '…': '...',
    '×': 'x', '·': '.', '■': '#', '▬': '-', '█': '#',
    '─': '-', '━': '-', '═': '=', '♥': '<3', '❤': '<3',
    '⬛': '#', '●': '*', '★': '*', '☆': '*', '➜': '>',
    '»': '>>', '«': '<<', '→': '->', '←': '<-',
};

function fold(s) {
    let out = '';
    for (const ch of String(s)) {
        const c = ch.codePointAt(0);
        if (ch === SECTION) { out += ch; continue; }
        if (c >= 32 && c <= 126) { out += ch; continue; }
        // Newlines survive the fold and are split on afterwards: the plugin
        // embeds them in its match-stats broadcasts, and the console's chat
        // ring is one line per entry.
        if (c === 10) { out += '\n'; continue; }
        if (c === 13) continue;
        if (c === 9) { out += ' '; continue; }
        const sub = FOLD[ch];
        if (sub !== undefined) { out += sub; continue; }
        // Strip combining marks off accented Latin rather than dropping the letter.
        const nfd = ch.normalize('NFD').replace(/[̀-ͯ]/g, '');
        if (nfd.length === 1 && nfd.codePointAt(0) >= 32 && nfd.codePointAt(0) <= 126) {
            out += nfd;
        }
    }
    return out;
}

/**
 * A folded string to the wire form: the line's first colour as a byte (0..15,
 * 0xff for none) and the text with every code removed. One colour per line is
 * a real loss on MegaSkywars' heavily formatted chat, but the alternative is
 * a per-span protocol for text the player reads for a second and dismisses.
 */
function toWire(s) {
    let colour = 0xff;
    let out = '';
    for (let i = 0; i < s.length; i++) {
        if (s[i] !== SECTION || i + 1 >= s.length) { out += s[i]; continue; }
        const code = s[++i].toLowerCase();
        const idx = COLOUR_CODES.indexOf(code);
        if (idx >= 0 && colour === 0xff) colour = idx;   // first colour wins
    }
    return { colour, text: out };
}

/**
 * Everything at once: a raw S02 message field (JSON string or already-parsed
 * component) to { colour, text, lines }. The plugin embeds newlines in its
 * match-stats broadcasts, and the console's chat ring is line-based.
 */
function parse(message) {
    let component = message;
    if (typeof message === 'string') {
        try { component = JSON.parse(message); }
        catch (_) { component = { text: message }; }
    }
    const raw = fold(toText(component));
    const lines = raw.split(/\r?\n/).map(toWire).filter((l) => l.text.trim().length);
    const flat = toWire(raw.replace(/\r?\n/g, ' '));
    return { colour: flat.colour, text: flat.text, lines };
}

module.exports = { parse, toText, fold, toWire, format, TRANSLATIONS, COLOUR_NAMES };
