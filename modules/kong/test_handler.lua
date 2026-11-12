-- Unit test for handler.lua parse logic.
-- Run: lua test_handler.lua
--
-- Tests the Accept-Encoding parser without requiring a Kong runtime.

-- Inline copy of the parse function (kept in sync with handler.lua).
local function parse_accept_encoding(ae)
    if not ae or not ae:find("pidgin", 1, true) then return false, nil end
    local fp = ae:match("pidgin%s*;%s*profile%s*=%s*([%w]+)")
    return true, fp
end

local function unquote_etag(s)
    if not s then return nil end
    if s:sub(1,1) == '"' and s:sub(-1) == '"' then return s:sub(2, -2) end
    return s
end

local cases = {
    -- {input, expected_wants, expected_fp, description}
    {nil, false, nil, "nil header"},
    {"", false, nil, "empty header"},
    {"gzip, br", false, nil, "no pidgin"},
    {"pidgin", true, nil, "bare pidgin"},
    {"gzip, pidgin", true, nil, "pidgin among others"},
    {"pidgin; profile=abc123", true, "abc123", "pidgin with fingerprint"},
    {"pidgin;profile=abc123", true, "abc123", "no space before semicolon"},
    {"pidgin;  profile  =  abc123  ", true, "abc123", "extra whitespace"},
    {"gzip, pidgin; profile=deadbeef, br", true, "deadbeef", "pidgin among others with fp"},
    {"pidgin; q=0.5", true, nil, "pidgin with q-value, no fingerprint"},
}

local function fmt(v)
    if v == nil then return "nil" end
    if type(v) == "boolean" then return tostring(v) end
    return string.format("%q", v)
end

local passed, failed = 0, 0
for _, c in ipairs(cases) do
    local input, exp_wants, exp_fp, desc = c[1], c[2], c[3], c[4]
    local got_wants, got_fp = parse_accept_encoding(input)
    if got_wants == exp_wants and got_fp == exp_fp then
        passed = passed + 1
        print("  PASS: " .. desc)
    else
        failed = failed + 1
        print(string.format("  FAIL: %s\n    input=%s  expected=(%s, %s)  got=(%s, %s)",
            desc, fmt(input), fmt(exp_wants), fmt(exp_fp), fmt(got_wants), fmt(got_fp)))
    end
end

-- unquote_etag tests
local etag_cases = {
    {nil, nil},
    {"", ""},
    {'"abc123"', "abc123"},
    {"abc123", "abc123"},  -- already unquoted
    {'"deadbeef"', "deadbeef"},
}

print("\nETag unquoting:")
for _, c in ipairs(etag_cases) do
    local got = unquote_etag(c[1])
    if got == c[2] then
        passed = passed + 1
        print(string.format("  PASS: unquote(%s) = %s", fmt(c[1]), fmt(got)))
    else
        failed = failed + 1
        print(string.format("  FAIL: unquote(%s) expected=%s got=%s", fmt(c[1]), fmt(c[2]), fmt(got)))
    end
end

print(string.format("\n%d passed, %d failed", passed, failed))
os.exit(failed == 0 and 0 or 1)
