-- samples/japanese_ime_client.lua の簡単なテスト（ゲーム無しで動く）。
-- 使い方: リポジトリの一番上で  luajit tests/test_client.lua  （lua5.1 でも可）
package.path = './samples/?.lua;' .. package.path

local n = 0
local function eq(a, b, what)
    n = n + 1
    if a ~= b then error(string.format('%s: expected %s, got %s', what, tostring(b), tostring(a)), 2) end
end

-- 偽の環境 -------------------------------------------------------------------------------
local T = 0
local handlers, raised, ran, printed = {}, {}, {}, {}
local plugins = {}            -- vana.plugins.loaded() が返す名前
local chat_open = false
local use_vana = true
local ashita_loaded = false
local item_active = false
local has_add = true
local io_added = {}
local drawn = {}

_G.print = function (s) printed[#printed + 1] = tostring(s) end
_G.ashita = { events = {
    register = function (ev, alias, fn) handlers[ev] = fn end,
    unregister = function (ev, alias) handlers[ev] = nil end,
} }
local function vana_tbl()
    return {
        time = { clock = function () return T end },
        plugins = {
            loaded = function () local l = {} for _, p in ipairs(plugins) do l[#l + 1] = { name = p, source = 'ashita' } end return l end,
            raise = function (name, data) raised[#raised + 1] = name .. '=' .. data end,
        },
        chat = { run = function (line) ran[#ran + 1] = line end },
        game = { world = function () return { chat_open = chat_open } end },
    }
end
_G.vana = vana_tbl()
_G.AshitaCore = {
    GetPluginManager = function () return {
        IsLoaded = function (_, name) return ashita_loaded and name == 'japanese_ime' end,
        RaiseEvent = function (_, name, data, size) raised[#raised + 1] = 'A:' .. name .. '=' .. data end,
    } end,
    GetChatManager = function () return { QueueCommand = function (_, mode, line) ran[#ran + 1] = 'A:' .. line end } end,
}
setmetatable(AshitaCore, { __index = function () return nil end })
local imgui = {
    IsItemActive = function () return item_active end,
    GetTime = function () return T end,
    GetIO = function ()
        if not has_add then return {} end
        return { AddInputCharactersUTF8 = function (_, s) io_added[#io_added + 1] = s end }
    end,
    GetItemRectMin = function () return 0, 0 end,
    GetItemRectMax = function () return 100, 20 end,
    GetFontSize = function () return 10 end,
    GetStyle = function () return { FramePadding = { x = 4, y = 5 } } end,
    CalcTextSize = function (s) return #s * 2, 10 end,      -- 1 バイト 2 ドット
    GetForegroundDrawList = function () return {
        PushClipRect = function (_, a, b) drawn[#drawn + 1] = 'clip' end,
        PopClipRect = function () drawn[#drawn + 1] = 'pop' end,
        AddRectFilled = function () end,
        AddText = function (_, pos, col, s) drawn[#drawn + 1] = string.format('text %g,%g %s', pos[1], pos[2], s) end,
        AddLine = function (_, a, b) drawn[#drawn + 1] = string.format('line %g-%g y%g', a[1], b[1], a[2]) end,
    } end,
}
package.loaded['imgui'] = imgui

local function pev(name, data) handlers.plugin_event({ name = name, data = data, size = #data }) end
local buf = { '' }
local function frame(dt)
    T = T + (dt or 0.05)
    jime.field(imgui, buf, 64, '##s')
    jime.frame_end()
end

-- 名前の比べ方
_G.jime = require('japanese_ime_client')
eq(jime._norm('Japanese_IME'), 'japaneseime', 'normalize')
eq(jime._norm('japanese ime'), 'japaneseime', 'normalize spaces')
eq(jime._clip_utf8('あいう', 7), 'あい', 'clip utf8')

-- 1) プラグインが無い：何も送らず、1 回だけお知らせ
jime.setup('testaddon')
eq(type(handlers.plugin_event), 'function', 'plugin_event registered')
item_active = true
frame(); frame(); frame(3)
eq(#raised, 0, 'no plugin: nothing raised')
eq(#ran, 0, 'no plugin: no command')
eq(#printed, 1, 'one notice when the plugin is missing')
eq(printed[1]:find('読み込まれていません', 1, true) ~= nil, true, 'missing notice text')
item_active = false; frame()

-- 2) vana.plugins で見つかる（名前は大文字・_ 違いでも）→ on、返事あり、文字が入る
plugins = { 'Japanese_IME' }
item_active = true
frame(); frame()
eq(raised[#raised], 'japanese_ime_ctl=on testaddon', 'on via vana.plugins.raise')
pev('japanese_ime_state', 'on testaddon'); frame()
eq(jime.state().confirmed, true, 'confirmed by state')
pev('japanese_ime_text', 'yunaui\tよそ'); frame()
eq(#io_added, 0, 'text for another addon ignored')
pev('japanese_ime_text', 'testaddon\tクリスタル'); frame()
eq(io_added[1], 'クリスタル', 'inserted with AddInputCharactersUTF8')
buf = { 'ab' }
pev('japanese_ime_comp', 'testaddon\tかな'); drawn = {}; frame()
eq(jime.state().comp, 'かな', 'composition kept')
-- 欄の中、打ってある文字（ab = 4 ドット）の後ろ：x = 0 + 余白 4 + 4、y = 0 + 余白 5。下線は y + 10
eq(drawn[1], 'clip', 'clipped to the field')
eq(drawn[2], 'text 8,5 かな', 'drawn after the typed text inside the field')
eq(drawn[3], 'line 8-20 y15', 'underlined')
eq(drawn[4], 'pop', 'clip popped')
-- 長くて右端（100 - 4）からはみ出す → 右端にそろえる
buf = { string.rep('x', 40) }; drawn = {}; frame()
eq(drawn[2], 'text 84,5 かな', 'right-aligned when it would overflow')
buf = { '' }
pev('japanese_ime_comp', 'testaddon\t'); drawn = {}; frame()
eq(#drawn, 0, 'nothing drawn when composition ends')
frame(2)
eq(#ran, 0, 'confirmed: no fallback command')
-- チャット欄が開いても off は送らない（止めるのはプラグイン。持ち主は残る）
local before = #raised
chat_open = true; frame(); chat_open = false; frame()
eq(#raised, before, 'nothing sent when the game chat opens')
item_active = false; frame()
eq(raised[#raised], 'japanese_ime_ctl=off testaddon', 'off when the box is left')

-- 3) 返事が来ない → 黙ったコマンド、やめるときも
printed = {}
item_active = true; frame()
frame(0.5)
eq(#ran, 0, 'not yet')
frame(0.6)
eq(ran[#ran], '/jime addon on testaddon', 'fallback command after ANSWER_WAIT')
eq(#printed, 1, 'one notice: detected but no reply')
eq(printed[1]:find('返事がありません', 1, true) ~= nil, true, 'no-reply notice text')
-- 別のアドオンが持ち主になった返事は、自分の返事にならない
pev('japanese_ime_state', 'on yunaui'); frame()
eq(jime.state().confirmed, false, 'owner is another addon')
item_active = false; frame()
eq(ran[#ran], '/jime addon off testaddon', 'fallback off command')

-- 4) AddInputCharactersUTF8 が無い → buf の後ろに足し、次のフレームだけ別のラベル
has_add = false
buf = { 'abc' }
item_active = true; frame()
pev('japanese_ime_state', 'on testaddon')
pev('japanese_ime_text', 'testaddon\tあ'); T = T + 0.05
local changed = jime.field(imgui, buf, 64, '##s'); jime.frame_end()
eq(changed, true, 'buffer changed')
eq(buf[1], 'abcあ', 'appended to the buffer')
eq(jime.label('##s'), '##s_jime', 'other id for one frame')
frame()
eq(jime.label('##s'), '##s', 'normal id again')
has_add = true

-- 5) vana が無い（Ashita だけ）：IsLoaded / RaiseEvent / QueueCommand
item_active = false; frame()
_G.vana = nil
ashita_loaded = true
raised, ran = {}, {}
item_active = true; frame(); frame()
eq(raised[#raised], 'A:japanese_ime_ctl=on testaddon', 'on via RaiseEvent')
frame(1.2)
eq(ran[#ran], 'A:/jime addon on testaddon', 'fallback via QueueCommand')

-- 6) アンロード
jime.shutdown()
eq(raised[#raised], 'A:japanese_ime_ctl=off testaddon', 'off on shutdown')
eq(ran[#ran], 'A:/jime addon off testaddon', 'command off on shutdown')
eq(handlers.plugin_event, nil, 'unregistered')

io.stdout:write(string.format('test_client ok (%d checks)\n', n))
