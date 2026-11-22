--[[
  japanese_ime_client.lua — 日本語入力プラグイン japanese_ime を使うための、アドオン側の小さな部品（見本）
  作者: Tsukasa / ライセンス: GPL-3.0（プラグインと同じ）

  アドオンがするのは、「入力欄を使い始めた／使い終わった」とプラグインに知らせることと、
  届いた文字を欄に入れることだけです。チャット欄を閉じたままにすることと、
  IME が「A」（英数）に切り替わらないようにすることは、プラグインがします。
  書き足すのは次の 3 か所です。

    1) 読み込み時に 1 回:   local jime = require('japanese_ime_client'); jime.setup('youraddon')
    2) 入力欄のすぐ後ろで:  imgui.InputText('##search', buf, 256); jime.field(imgui)
    3) 毎フレームの最後に:  jime.frame_end()
    （アンロード時に jime.shutdown() も呼ぶと、オンのまま残りません）

  知らせの名前（plugin_event）
    アドオン → プラグイン  'japanese_ime_ctl'   "on <アドオンID>" / "off <アドオンID>"
    プラグイン → アドオン  'japanese_ime_text'  "<アドオンID>\t<確定した文字（UTF-8）>"
                           'japanese_ime_comp'  "<アドオンID>\t<変換中の文字>"（空なら変換が終わった）
                           'japanese_ime_state' "on <アドオンID>" / "off"
    文字を受け取るのは、最後に on にしたアドオン 1 つだけ。自分の ID が付いたものだけ使います。
]]

local M = {
    PLUGIN = 'japanese_ime',
    id = 'addon',          -- setup で決めるアドオンの ID（英小文字の短い名前）
    on = false,            -- プラグインに on を頼んでいるか
    active = false,        -- このフレームで、自分の入力欄が選ばれているか
    queue = {},            -- 届いた確定の文字（次に欄を描くときに入れる）
    comp = '',             -- 変換中の文字（見せるだけ）
}

-- プラグインが読み込まれているか
local function plugin_loaded()
    local ok, r = pcall(function () return AshitaCore:GetPluginManager():IsLoaded(M.PLUGIN) end)
    return ok and r == true
end

-- プラグインへ知らせる
local function raise(name, data)
    pcall(function () AshitaCore:GetPluginManager():RaiseEvent(name, data, #data) end)
end

-- ゲームのチャット欄が開いているか（VanaCore にあれば使う。分からなければ false）
local function game_chat_open()
    local ok, r = pcall(function () return vana.game.world().chat_open end)
    return ok and r == true
end

-- プラグインからの知らせを受ける
local function on_plugin_event(e)
    if type(e) ~= 'table' or type(e.name) ~= 'string' or e.name:sub(1, 12) ~= 'japanese_ime' then return end
    local data = e.data
    if type(data) ~= 'string' and e.data_raw ~= nil and tonumber(e.size) then
        local ok, s = pcall(function () return require('ffi').string(e.data_raw, tonumber(e.size)) end)
        if ok then data = s end
    end
    if type(data) ~= 'string' then return end
    data = data:gsub('%z.*$', '')
    local owner, text = data:match('^([^\t]*)\t(.*)$')
    if owner ~= M.id then return end                  -- ほかのアドオンあての文字
    if e.name == 'japanese_ime_text' then
        M.queue[#M.queue + 1] = text
    elseif e.name == 'japanese_ime_comp' then
        M.comp = text
    end
end

function M.setup(addon_id)
    M.id = addon_id or M.id
    ashita.events.register('plugin_event', M.id .. '_japanese_ime', on_plugin_event)
end

-- 入力欄（InputText など）を描いたすぐ後に呼ぶ
function M.field(imgui)
    if not imgui.IsItemActive() then return end
    M.active = true
    -- 確定した文字を、ImGui の入力として渡す（カーソルの位置に入る）
    if #M.queue > 0 then
        local io = imgui.GetIO()
        for _, text in ipairs(M.queue) do io:AddInputCharactersUTF8(text) end
        M.queue = {}
    end
    -- 変換中の文字を欄のすぐ下に出す（任意。要らなければ消してかまいません）
    if M.comp ~= '' then
        local x, _ = imgui.GetItemRectMin()
        local _, y = imgui.GetItemRectMax()
        imgui.GetForegroundDrawList():AddText({ x + 4, y + 2 }, 0xFFFFFFFF, M.comp)
    end
end

-- 毎フレームの最後に呼ぶ：欄が選ばれた／外れたときだけ on／off を送る
function M.frame_end()
    local want = M.active and not game_chat_open() and plugin_loaded()
    M.active = false
    if want == M.on then return end
    M.on = want
    raise('japanese_ime_ctl', (want and 'on ' or 'off ') .. M.id)
    if not want then M.queue = {}; M.comp = '' end
end

function M.shutdown()
    if M.on then raise('japanese_ime_ctl', 'off ' .. M.id) end
    M.on = false
end

return M
