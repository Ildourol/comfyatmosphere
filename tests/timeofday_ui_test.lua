-- Run the actual time controls with a small WoW UI stand-in. No game required.
local file = assert(io.open(arg[1], "r"));
local source = file:read("*a");
file:close();
local function between(first, last)
    local start = assert(string.find(source, first, 1, true));
    local finish = assert(string.find(source, last, start, true));
    return string.sub(source, start, finish - 1);
end

math.mod = math.mod or math.fmod;
table.getn = table.getn or function(t) return #t; end;
getglobal = function(name) return _G[name]; end;
local methods = {};
for _, name in ipairs({ "SetWidth", "SetHeight", "SetPoint", "SetMinMaxValues",
    "SetValueStep", "SetJustifyH" }) do
    methods[name] = function() end;
end
function methods:SetText(value) self.text = value; end
function methods:SetChecked(value) self.checked = value; end
function methods:GetChecked() return self.checked; end
function methods:GetName() return self.name; end
function methods:SetScript(event, handler) self.scripts[event] = handler; end
function methods:EnableMouse(value) self.mouse = value; end
function methods:SetAlpha(value) self.alpha = value; end
function methods:Enable() self.enabled = true; end
function methods:Disable() self.enabled = false; end
function methods:GetValue() return self.value; end
function methods:SetValue(value)
    self.value = value;
    if self.scripts.OnValueChanged then
        local previous = this;
        this = self;
        self.scripts.OnValueChanged();
        this = previous;
    end
end
function CreateFrame(kind, name, parent, template)
    local frame = setmetatable({scripts = {}, enabled = true, name = name}, {__index = methods});
    if name then
        _G[name] = frame;
        if template then
            _G[name .. "Text"] = CreateFrame();
            if kind == "Slider" then
                _G[name .. "Low"] = CreateFrame();
                _G[name .. "High"] = CreateFrame();
            end
        end
    end
    return frame;
end
function methods:CreateFontString() self.fontString = CreateFrame(); return self.fontString; end
GetGameTime = function() return 21, 56; end;
local shownHour, shownState;
TimeShown = function() return shownHour, shownState; end;
local sent = {};
TimeSend = function(verb, value) table.insert(sent, {verb, value}); end;

local compile = loadstring or load;
local build, refresh = assert(compile(
    between("local function TimeText(", "-- A held key") ..
    between("-- The Time section of the settings window", "-- The end of the Time section.") ..
    "return TimeSectionBuild, TimeSectionRefresh"
))();
local parent = CreateFrame("Frame", "Parent");
local bottom = build(parent, -4, 198);
assert(bottom < -4, "the section takes room");
local function show(hour, state, line)
    shownHour, shownState = hour, state;
    refresh();
    assert(parent.fontString.text == line, "status line: " .. tostring(parent.fontString.text));
end
local function fixed(enabled)
    assert(ComfyAtmosphereTimeHour.mouse == enabled, "slider mode");
    for _, name in ipairs({"Earlier", "Later", "DayNight"}) do
        assert(_G["ComfyAtmosphereTime" .. name].enabled == enabled, "button mode");
    end
end
local function ticked(which)
    for _, name in ipairs({"Server", "Fixed", "Computer"}) do
        assert(_G["ComfyAtmosphereTime" .. name].checked == (name == which), "ticked " .. tostring(which));
    end
end
local function click(name, verb, value)
    local before = #sent;
    this = _G["ComfyAtmosphereTime" .. name];
    this:SetChecked(not this.checked);
    this.scripts.OnClick();
    if verb then
        assert(#sent == before + 1 and sent[#sent][1] == verb and sent[#sent][2] == value, "mode command " .. name);
    else
        assert(#sent == before, "no command for the mode already shown");
    end
    ticked(name);
end

assert(#sent == 0, "building must not issue time commands");
show(22.2625, 4, "Computer time: 22:15"); -- 22:15:45 must not round up to 22:16.
ticked("Computer");
fixed(false);
show(13.25, 1, "Fixed hour: 13:15");
ticked("Fixed");
fixed(true);
show(13, 0, "Server time: 21:56");
ticked("Server");
fixed(false);
for _, state in ipairs({2, 3}) do
    show(13, state, state == 2 and "comfytime.dll sets the time. Remove it" or "Not set (another WoW.exe?). See the log");
    ticked(nil);
    fixed(false);
end
show(nil, nil, "Waiting for comfyatmos.dll");
ticked(nil);
fixed(false);
assert(#sent == 0, "refresh must not issue time commands");

show(13, 0, "Server time: 21:56");
click("Computer", "local", "1");
show(22.25, 4, "Computer time: 22:15");
click("Server", "lock", "0 21.9333");
show(13, 0, "Server time: 21:56");
click("Fixed", "lock", "1");
show(13, 1, "Fixed hour: 13:00");
click("Fixed", nil);
ComfyAtmosphereTimeHour.scripts.OnMouseDown();
ComfyAtmosphereTimeHour:SetValue(6.5);
assert(sent[#sent][1] == "set" and sent[#sent][2] == "6.5000", "slider sets the hour");
this = ComfyAtmosphereTimeDayNight;
this.scripts.OnClick();
assert(sent[#sent][1] == "daynight", "Day/Night");
print("PASS: server/fixed/computer/error states, mode commands, slider and buttons, refresh guard");
