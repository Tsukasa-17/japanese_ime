--[[
  japanese_ime_client.lua — 日本語入力プラグイン japanese_ime を使うための、アドオン側の部品（見本）
  作者: Tsukasa / ライセンス: GPL-3.0（プラグインと同じ）

  置き方: このファイルをアドオンのフォルダに japanese_ime_client.lua という名前で置き、require で読みます。

  アドオンがするのは、「入力欄を使い始めた／使い終わった」とプラグインに知らせることと、
  届いた文字を欄に入れることだけです。チャット欄を閉じたままにすることと、
  IME が「A」（英数）に切り替わらないようにすることは、プラグインがします。
  書き足すのは次の 4 か所です。

    1) 読み込み時に 1 回:
         local jime = require('japanese_ime_client')
         jime.setup('youraddon')                  -- アドオンの ID（英小文字の短い名前）
    2) 入力欄を描くとき（ラベルは jime.label で包む。buf は { 文字 } の表）:
         imgui.InputText(jime.label('##search'), buf, 256)
         if jime.field(imgui, buf, 256, '##search') then print(buf[1]) end   -- 日本語が入って buf[1] が変わった
       （buf を渡さない jime.field(imgui) でも動きます。そのときは下の「文字の入れ方」の 2 つ目は使えません）
    3) 毎フレームの最後に:  jime.frame_end()
    4) アンロード時に:      jime.shutdown()

  プラグインとのやりとり（plugin_event）
    アドオン → プラグイン  'japanese_ime_ctl'   "on <ID>" / "off <ID>"
    プラグイン → アドオン  'japanese_ime_text'  "<ID>\t<確定した文字（UTF-8）>"
                           'japanese_ime_comp'  "<ID>\t<変換中の文字>"（空なら変換が終わった）
                           'japanese_ime_state' "on <いま文字を受け取っている ID>" / "off"
    文字を受け取るのは、最後に on にしたアドオン 1 つだけ。自分の ID が付いたものだけ使います。

  見つけ方・送り方
    - 読み込まれているか: VanaCore の vana.plugins.loaded() を先に見て（名前は小文字にし、_ と空白を除いて
      'japaneseime' と比べる）、無ければ Ashita の AshitaCore:GetPluginManager():IsLoaded('japanese_ime')。
    - 送る: vana.plugins.raise を先に、無ければ AshitaCore:GetPluginManager():RaiseEvent。
    - 返事: on を送ったあと、'japanese_ime_state' に "on <自分の ID>" が来れば届いている。
      ANSWER_WAIT 秒たっても来なければ、チャットに何も出ないコマンド '/jime addon on <ID>' でも頼む
      （やめるときは '/jime addon off <ID>'）。

  文字の入れ方
    1. ImGui の io:AddInputCharactersUTF8 があれば、それで入れる（カーソルの位置に入る）。
    2. 無ければ buf[1] の後ろに足し、ImGui が持っている古い文字に戻されないよう、
       次の 1 フレームだけ jime.label が別の ID（ラベル .. '_jime'）を返す。
]]

local M = {
    PLUGIN      = 'japanese_ime',
    PLUGIN_KEY  = 'japaneseime',   -- 名前を小文字にして _ と空白を除いたもの
    EV_CTL      = 'japanese_ime_ctl',
    EV_TEXT     = 'japanese_ime_text',
    EV_COMP     = 'japanese_ime_comp',
    EV_STATE    = 'japanese_ime_state',
    ANSWER_WAIT = 1.0,             -- on の返事を待つ秒数。過ぎたらコマンドでも頼む
    RECHECK     = 2.0,             -- 欄を選んでいる間、プラグインが読み込まれたかを見直す間隔（秒）
    notify      = true,            -- 状態を 1 回だけチャットに出すか（setup の opts.notify で変えられる）
    id          = 'addon',
}

local st

local function reset()
    st = {
        active = false,       -- このフレームで自分の欄が選ばれている
        active_label = nil,   -- そのラベル
        on = false,           -- プラグインに on を頼んでいる
        confirmed = false,    -- "on <自分の ID>" の返事が来た
        via_cmd = false,      -- 返事が無いので、コマンドでも頼んだ
        on_at = nil,          -- on を送った時刻
        loaded = nil,         -- プラグインが読み込まれているか（nil は未確認）
        checked_at = nil,
        queue = {},           -- 届いた確定の文字
        comp = '',            -- 変換中の文字
        reid = nil,           -- このフレームだけ別の ID で描く欄
        reid_next = nil,
        told = {},            -- 出したお知らせ（1 回だけ）
    }
end
reset()

-- 時刻（秒）。VanaCore の vana.time.clock、無ければ ImGui の時刻、最後に os.clock
function M.now()
    local ok, t = pcall(function () return vana.time.clock() end)
    if ok and type(t) == 'number' then return t end
    ok, t = pcall(function () return require('imgui').GetTime() end)
    if ok and type(t) == 'number' then return t end
    return os.clock()
end

local function norm(name)
    return (tostring(name or ''):lower():gsub('[_%s]', ''))
end
M._norm = norm

-- プラグインが読み込まれているか（true / false。どちらの窓口も無ければ nil）
function M.plugin_loaded()
    local ok, list = pcall(function () return vana.plugins.loaded() end)
    if ok and type(list) == 'table' then
        for _, p in ipairs(list) do
            local n = type(p) == 'table' and p.name or p
            if norm(n) == M.PLUGIN_KEY then return true end
        end
        return false
    end
    local ok2, r = pcall(function () return AshitaCore:GetPluginManager():IsLoaded(M.PLUGIN) end)
    if ok2 and r ~= nil then return r == true end
    return nil
end

-- プラグインへ知らせる
function M.raise(name, data)
    data = data or ''
    if pcall(function () vana.plugins.raise(name, data) end) then return true end
    return (pcall(function () AshitaCore:GetPluginManager():RaiseEvent(name, data, #data) end))
end

-- チャットに何も出ないコマンドを打つ（/jime addon on|off <ID>）
function M.command(line)
    if pcall(function () vana.chat.run(line) end) then return true end
    return (pcall(function () AshitaCore:GetChatManager():QueueCommand(-1, line) end))
end

local function tell(key, text)
    if not M.notify or st.told[key] then return end
    st.told[key] = true
    print(string.format('[%s] %s', M.id, text))
end

-- プラグインからの知らせ（plugin_event）
function M.on_event(name, data)
    if type(name) ~= 'string' or name:sub(1, 12) ~= 'japanese_ime' then return false end
    data = type(data) == 'string' and data or ''
    if name == M.EV_STATE then
        st.confirmed = (data == 'on ' .. M.id)
        if st.confirmed then st.told.noreply = nil end
        return true
    end
    local owner, text = data:match('^([^\t]*)\t(.*)$')
    if owner ~= M.id then return true end             -- ほかのアドオンあて
    if name == M.EV_TEXT then
        if st.on and text ~= '' then st.queue[#st.queue + 1] = text end
    elseif name == M.EV_COMP then
        st.comp = st.on and text or ''
    end
    return true
end

local function on_plugin_event(e)
    if type(e) ~= 'table' then return end
    local data = e.data
    local size = tonumber(e.size)
    if type(data) ~= 'string' and e.data_raw ~= nil and size and size > 0 then
        local ok, s = pcall(function () return require('ffi').string(e.data_raw, size) end)
        if ok then data = s end
    end
    if type(data) ~= 'string' then data = '' end
    if size and size >= 0 and #data > size then data = data:sub(1, size) end
    M.on_event(e.name, (data:gsub('%z.*$', '')))
end

function M.setup(addon_id, opts)
    M.id = addon_id or M.id
    if type(opts) == 'table' and opts.notify ~= nil then M.notify = opts.notify and true or false end
    reset()
    pcall(function () ashita.events.register('plugin_event', M.id .. '_japanese_ime', on_plugin_event) end)
end

-- 入力欄のラベル（文字を後ろに足した次のフレームだけ別の ID）
function M.label(label)
    if st.reid == label then return label .. '_jime' end
    return label
end

-- ImGui の入力として渡す。できなければ false
local function feed(imgui, text)
    local ok, io = pcall(imgui.GetIO)
    if not ok or io == nil then return false end
    local okf, fn = pcall(function () return io.AddInputCharactersUTF8 end)
    if not okf or type(fn) ~= 'function' then return false end
    return (pcall(fn, io, text))
end

-- UTF-8 の途中で切らずに max バイトまで
local function clip_utf8(s, max)
    if #s <= max then return s end
    local cut = max
    while cut > 0 do
        local b = s:byte(cut + 1)
        if not b or b < 0x80 or b >= 0xC0 then break end
        cut = cut - 1
    end
    return s:sub(1, cut)
end
M._clip_utf8 = clip_utf8

-- 入力欄を描いたすぐ後に呼ぶ。buf（{ 文字 }）・size・label は、文字の入れ方 2 を使うときだけ要る。
-- buf[1] を変えたら true
function M.field(imgui, buf, size, label)
    local ok, a = pcall(imgui.IsItemActive)
    if not ok or a ~= true then return false end
    st.active = true
    st.active_label = label
    local changed = false
    if #st.queue > 0 then
        local text = table.concat(st.queue)
        st.queue = {}
        if not feed(imgui, text) and type(buf) == 'table' then
            local nb = clip_utf8((buf[1] or '') .. text, (tonumber(size) or 256) - 1)
            if nb ~= buf[1] then buf[1] = nb; changed = true end
            if label then st.reid_next = label end
        end
    end
    -- 変換中の文字を欄のすぐ下に出す（任意。要らなければ消してかまいません）
    if st.comp ~= '' then
        pcall(function ()
            local x = imgui.GetItemRectMin()
            local _, y = imgui.GetItemRectMax()
            imgui.GetForegroundDrawList():AddText({ x + 4, y + 2 }, 0xFFFFFFFF, st.comp)
        end)
    end
    return changed
end

-- 毎フレームの最後に呼ぶ：欄が選ばれた／外れたときだけ on／off を送る
function M.frame_end()
    local now = M.now()
    st.reid = st.reid_next
    st.reid_next = nil
    local active = st.active
    st.active = false
    -- ゲームのチャット欄が開いている間はプラグインが自分で取得を止めるので、ここでは見ない
    local want = active
    if want then
        if st.loaded == nil or st.checked_at == nil or now - st.checked_at >= M.RECHECK or now < st.checked_at then
            st.checked_at = now
            st.loaded = M.plugin_loaded()
        end
        if st.loaded ~= true then
            tell('missing', 'japanese_ime プラグインが読み込まれていません。日本語を入れるには /plugin load japanese_ime（英数字はそのまま打てます）')
        end
    else
        st.loaded, st.checked_at = nil, nil
    end
    local target = want and st.loaded == true
    if target ~= st.on then
        st.on = target
        if target then
            st.confirmed, st.via_cmd, st.on_at = false, false, now
            M.raise(M.EV_CTL, 'on ' .. M.id)
        else
            M.raise(M.EV_CTL, 'off ' .. M.id)
            if st.via_cmd then M.command('/jime addon off ' .. M.id) end
            st.via_cmd, st.on_at, st.confirmed = false, nil, false
            st.queue, st.comp = {}, ''
        end
    elseif target and not st.confirmed and not st.via_cmd and st.on_at and now - st.on_at >= M.ANSWER_WAIT then
        -- 知らせが届かない（返事が無い）：黙ったコマンドでも頼む
        st.via_cmd = true
        M.command('/jime addon on ' .. M.id)
        tell('noreply', 'japanese_ime は読み込まれていますが、知らせに返事がありません。コマンドで頼みました（/jime status で確かめられます）')
    end
end

function M.shutdown()
    if st.on then
        M.raise(M.EV_CTL, 'off ' .. M.id)
        if st.via_cmd then M.command('/jime addon off ' .. M.id) end
    end
    pcall(function () ashita.events.unregister('plugin_event', M.id .. '_japanese_ime') end)
    reset()
end

-- 確かめ用
function M.state() return st end

return M
