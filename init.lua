-- mod-version:2 -- lite-xl 2.0
local core = require "core"
local keymap = require "core.keymap"
local command = require "core.command"
local common = require "core.common"
local config = require "core.config"
local style = require "core.style"
local View = require "core.view"

local process = require "process"

-- Import TMT; override CPATH to include DATADIR and USERDIR
local soname = PLATFORM == "Windows" and "?.dll" or "?.so"
local cpath = package.cpath
package.cpath = DATADIR .. '/plugins/tmt/' .. soname .. ';' .. package.cpath
package.cpath = USERDIR .. '/plugins/tmt/' .. soname .. ';' .. package.cpath
local luaterm = require "luaterm"
package.cpath = cpath


local merge = common.merge or function(a, b)
    local t = {}
    for k, v in pairs(a) do t[k] = v end
    if b then for k, v in pairs(b) do t[k] = v end end
    return t
end


local ESC = "\x1b"
local VBEL_DURATION = 0.2
local PASSTHROUGH_PATH = USERDIR .. "/plugins/tmt/pty"
local TERMINATION_MSG = "\r\n\n[Process ended with status %d]"
local COLORS = {
    { common.color "#000000" },
    { common.color "#cc0000" },
    { common.color "#4e9a06" },
    { common.color "#c4a000" },
    { common.color "#3465a4" },
    { common.color "#75507b" },
    { common.color "#06989a" },
    { common.color "#d3d7cf" },
    { common.color "#555753" },
    { common.color "#ef2929" },
    { common.color "#8ae234" },
    { common.color "#fce94f" },
    { common.color "#729fcf" },
    { common.color "#ad7fa8" },
    { common.color "#34e2e2" },
    { common.color "#eeeeec" },
    
    style.text,
    style.background
}

local conf = merge({
    shell = os.getenv(PLATFORM == "Windows" and "COMSPEC" or "SHELL") or "/bin/sh",
    shell_args = {},
    split_direction = "down",
    resize_interval = 0.3, -- in seconds
    palette = COLORS,
    scrollback_size = 9999,
    audio_bell = true,
    visual_bell = true,
}, config.plugins.tmt)


local TmtView = View:extend()

function TmtView:new()
    TmtView.super.new(self)
    self.scrollable = false

    local args = { PASSTHROUGH_PATH, conf.shell }
    for _, arg in ipairs(conf.shell_args) do
        table.insert(args, arg)
    end
    self.proc = assert(process.start(args, {
        stdin = process.REDIRECT_PIPE,
        stdout = process.REDIRECT_PIPE
    }))

    self.tmt = luaterm.tsm.new(
        24,
        80,
        conf.scrollback_size,
        function(...) self:on_tsm_event(...) end
    )
    self.tmt:set_palette(conf.palette)

    self.title = "Tmt"
    self.visible = true
    self.scroll_region_start = 1
    self.scroll_region_end = self.rows

    self.term_target_size = { w = 80, h = 24 }

    core.add_thread(function()
        while true do
            local ok, output = pcall(self.proc.read_stdout, self.proc)
            if not ok or not output then break end

            if #output > 0 then
                core.redraw = true
                local answers = self.tmt:write(output)
                for _, ans in ipairs(answers) do
                    self:input_string(ans)
                end
            end
            coroutine.yield(1 / config.fps)
        end

        self.tmt:write(string.format(TERMINATION_MSG, self.proc:returncode() or 0))
    end, self)
end

function TmtView:try_close(...)
    self.proc:kill()
    TmtView.super.try_close(self, ...)
end

function TmtView:get_name()
    return self.title
end

function TmtView:on_tsm_event(event, data)
    if event == "bel" then
        if conf.audio_bell then
            luaterm.bel(type(conf.audio_bell) == "string" and conf.audio_bell or nil)
        end
        if conf.visual_bell then
            self.vbel_start = system.get_time()
        end
    end
end

function TmtView:update(...)
    TmtView.super.update(self, ...)

    local sw, sh = self:get_screen_char_size()
    local tw, th = self.tmt:get_size()
    if sw ~= tw or sh ~= th then
        self.term_target_size.w, self.term_target_size.h = sw, sh
        if not self.resize_start then
            self.resize_start = system.get_time()
        end
    end

    if self.resize_start
        and (system.get_time() - self.resize_start > conf.resize_interval) then
        self.resize_start = nil
        self.tmt:set_size(sh, sw)
        self:input_string(string.format("\x1bXP%d;%dR\x1b\\", sh, sw))
    end

    -- update blink timer
    if self == core.active_view then
        local T, t0 = config.blink_period, core.blink_start
        local ta, tb = core.blink_timer, system.get_time()
        if ((tb - t0) % T < T / 2) ~= ((ta - t0) % T < T / 2) then
            core.redraw = true
        end
        core.blink_timer = tb
    end

    if self.vbel_start and system.get_time() - self.vbel_start >= VBEL_DURATION then
        self.vbel_start = nil
    end
end

function TmtView:on_text_input(text)
    self:input_string(text)
end

function TmtView:input_string(str)
    if not self.proc:running() then
        return command.perform "root:close"
    end
    pcall(self.proc.write, self.proc, str)
end

function TmtView:get_screen_char_size()
    local font = style.code_font
    local x = self.size.x - style.padding.x
    local y = self.size.y - style.padding.y
    return math.max(1, math.floor(x / font:get_width("A"))),
        math.max(1, math.floor(y / font:get_height()))
end

local function invert(color)
    local c = {}
    for i, v in ipairs(color) do
        c[i] = math.abs(255 - v)
    end
    return c
end

local invisible = { ["\r"] = true, ["\n"] = true, ["\v"] = true, ["\t"] = true, ["\f"] = true, [" "] = true }
function TmtView:draw()
    local font = style.code_font
    local ox,oy = self:get_content_offset()
    local fw, fh = font:get_width("A"), font:get_height()

    self:draw_background(style.background)

    -- render screen
    ox, oy = ox + style.padding.x, oy + style.padding.y
    self.tmt:draw(function(rects, textruns)
        for _, r in ipairs(rects) do
            renderer.draw_rect(
                ox + r.x * fw,
                oy + r.y * fh,
                r.w * fw,
                fh,
                { r.r, r.g, r.b }
            )
        end
        for _, t in ipairs(textruns) do
            renderer.draw_text(
                style.code_font,
                t.text,
                ox + t.x * fw,
                oy + t.y * fh,
                { t.r, t.g, t.b }
            )
        end
    end)

    -- render caret
    core.blink_timer = system.get_time()
    local T = config.blink_period
    if system.window_has_focus() then
        if config.disable_blink
            or (core.blink_timer - core.blink_start) % T < T / 2 then
            local cy, cx = self.tmt:get_cursor()
            local x, y = ox + (cx - 1) * fw, oy + (cy - 1) * fh
            renderer.draw_rect(x, y, style.caret_width, fh, style.caret)
        end
    end

    ox, oy = self:get_content_offset()
    if self.vbel_start then
        local color = invert(style.background)
        color[4] = 255
        renderer.draw_rect(ox,                                 oy, style.padding.y, self.size.y, color)
        renderer.draw_rect(ox + self.size.x - style.padding.y, oy, style.padding.y, self.size.y, color)
        renderer.draw_rect(ox, oy,                                 self.size.x, style.padding.y, color)
        renderer.draw_rect(ox, oy + self.size.y - style.padding.y, self.size.x, style.padding.y, color)
    end
end

-- override input handling

local macos = PLATFORM == "Mac OS X"
local modkeys_os = require("core.modkeys-" .. (macos and "macos" or "generic"))
local modkey_map = modkeys_os.map
local modkeys = modkeys_os.keys

local keymap_on_key_pressed = keymap.on_key_pressed
function keymap.on_key_pressed(k, ...)
    if not core.active_view:is(TmtView) then
        return keymap_on_key_pressed(k, ...)
    end

    local mk = modkey_map[k]
    if mk then
        -- keymap_on_key_pressed(k)
        keymap.modkeys[mk] = true
        -- work-around for windows where `altgr` is treated as `ctrl+alt`
        if mk == "altgr" then
            keymap.modkeys["ctrl"] = false
        end
    else
        local actions = {
            ["return"] = "\r",
            ["up"] = ESC .. "OA",
            ["down"] = ESC .. "OB",
            ["right"] = ESC .. "OC",
            ["left"] = ESC .. "OD",
            ["backspace"] = "\x7f",
            ["escape"] = "\x1b",
            ["tab"] = "\t",
            ["space"] = " ",
        }
        if actions[k] then
            core.active_view:input_string(actions[k])
            return true
        elseif keymap.modkeys["ctrl"] then
            local char = string.byte(k) - string.byte('a') + 1
            core.active_view:input_string(string.char(char))
            return true
        else
            return false
        end
    end
end

local keymap_on_key_released = keymap.on_key_released
function keymap.on_key_released(k)
    local mk = modkey_map[k]
    if mk then
        keymap_on_key_released(k)
        keymap.modkeys[mk] = false
    end
end


-- this is a shared session used by tmt:view
-- it is not touched by "tmt:open-here"
local shared_view = nil
local function shared_view_exists()
    return shared_view and core.root_view.root_node:get_node_for_view(shared_view)
end
command.add(nil, {
    ["tmt:new"] = function()
        local node = core.root_view:get_active_node()
        if not shared_view_exists() then
            shared_view = TmtView()
        end
        node:split(conf.split_direction, shared_view)
        core.set_active_view(shared_view)
    end,
    ["tmt:toggle"] = function()
        if not shared_view_exists() then
            command.perform "tmt:new"
        else
            shared_view.visible = not shared_view.visible
            core.set_active_view(shared_view)
        end
    end,
    ["tmt:open-here"] = function()
        local node = core.root_view:get_active_node()
        node:add_view(TmtView())
    end
})

keymap.add({
    ["ctrl+t"] = "tmt:new",
    ["ctrl+`"] = "tmt:toggle"
})
