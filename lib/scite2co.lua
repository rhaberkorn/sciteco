#!/usr/bin/lua

-- string property storage
-- this is what SciTE uses internally
local props = {}

-- Recursively expand property references like "$(property)"
-- This is done lazily as in SciTE, i.e. not
-- when loading property sets
function expand(str)
	return str and str:gsub("$(%b())", function(ref)
		-- strips braces and expands nested references
		return expand(props[expand(ref:sub(2, -2))]) or ""
	end)
end

-- SciTE property files use properties like
-- keywords.$(file.patterns.lua)
-- When looking for a list of keywords, it inefficiently
-- evaluates all properties beginning with "keywords.", expands
-- the suffix and globs the current file name against the pattern.
-- We have no choice than to emulate that behaviour.
-- However SciTECO does not rematch the current file name against
-- the pattern for every SCI_SETKEYWORDS, so we try to check if
-- the expanded suffix contains the file pattern itself
function get_property_by_pattern(prefix, pattern)
	for key, value in pairs(props) do
		if key:sub(1, #prefix) == prefix and
		   expand(key:sub(1+#prefix)):find(pattern, 1, true) then
			return value
		end
	end
	-- if property not found, return nil
end

function load_properties(filename)
	local file = io.open(filename, "r")
	local line = file:read()

	while line do
		-- strip leading white space
		line = line:gsub("^%s", "")

		-- strip comments
		line = line:gsub("^#.*", "")

		-- read continuations
		while line:sub(-1) == "\\" do
			line = line:sub(1, -2)..(file:read() or "")
		end

		if line:find("^if ") then
			-- ignore conditionals
			repeat
				line = file:read()
			until not line or line:sub(1, 1) ~= "\t"
		else
			-- process assignments
			-- property references are not yet expanded,
			-- since SciTE relies on lazy evaluation
			local key, value = line:match("^([^=]+)=(.*)")
			if key then props[key] = value end

			line = file:read()
		end
	end

	file:close()
end

if #arg ~= 2 then
	io.stderr:write("Usage: ./scite2co.lua <language> <property file>\n\n"..
	                "Auto-generates a SciTECO lexer configuration from a SciTE\n"..
	                "properties file and prints it to stdout.\n\n"..
	                "Example: ./scite2co.lua lua Embedded.properties\n")
	os.exit(1)
end

-- language to extract is first command line argument
local language = arg[1]
table.remove(arg, 1)

-- load all property files given on the command line in order
for _, filename in ipairs(arg) do
	load_properties(filename)
end

io.write("! AUTO-GENERATED FROM SCITE PROPERTY SET !\n\n")

-- print [lexer.test...] macro
local shbang = expand(props["shbang."..language])
local file_patterns = expand(props["file.patterns."..language])
io.write([=[
@[lexer.test.]=]..language:lower()..[=[]{
]=])
if shbang then io.write([=[  _#!M]=]..shbang..[=[M[lexer.checkheader]"S -1 '
]=]) end
local patterns = {}
for pattern in file_patterns:gmatch("[^;]+") do
	table.insert(patterns, pattern)
end
for i, pattern in ipairs(patterns) do
	io.write([=[  :EN]=]..pattern..[=[Q*]=])
	if i ~= #patterns then io.write([=["S -1 ']=]) end
	io.write("\n")
end
io.write([=[}

]=])

-- print [lexer.set...] macro
-- NOTE: The lexer encoded in the property file is not
-- a SCLEX_* name but rather the lexer's module name
-- as set by the LexerModule constructor.
-- Therefore we must emit SCI_SETILEXER calls here.
local lexer = expand(get_property_by_pattern("lexer.", file_patterns))
io.write([=[
@[lexer.set.]=]..language:lower()..[=[]{
  ESSETILEXER]=]..lexer..[=[
]=])
-- print keyword definitions with word wrapping
for i = 1, 9 do
	local keyword_prefix = "keywords"..(i == 1 and "" or i).."."
	local value = expand(get_property_by_pattern(keyword_prefix, file_patterns))

	if value and #value > 0 then
		io.write("  "..(i-1).."ESSETKEYWORDS\n   ")
		local col = 3
		for word in value:gmatch("%g+") do
			col = col + 1 + #word
			if col > 80 then
				io.write("\n   ")
				col = 3
			end
			io.write(" "..word)
		end
		io.write("\n")
	end
end
-- print styles
-- SciTE has a single list of styles per Scintilla lexer which
-- is used by multiple languages - we output these definitions
-- for every language.
-- The SciTE colour settings are mapped to SciTECO color profiles
-- by heuristically analyzing SciTE's style definitions.
local style_mapping = {
	["font.comment"]		= "color.comment",
	["colour.code.comment.box"]	= "color.comment",
	["colour.code.comment.line"]	= "color.comment",
	["colour.code.comment.doc"]	= "color.comment",
	["colour.number"]		= "color.number",
	["colour.keyword"]		= "color.keyword",
	["colour.string"]		= "color.string",
	["colour.char"]			= "color.string2",
	["colour.preproc"]		= "color.preproc",
	["colour.operator"]		= "color.operator",
	["colour.error"]		= "color.error"
}
for i = 0, 255 do
	local value = props["style."..lexer.."."..i]

	if value then
		for p in value:gmatch("$(%b())") do
			local sciteco_color = style_mapping[p:sub(2, -2)]

			if sciteco_color then
				io.write("  :M["..sciteco_color.."],"..i.."M[color.set]\n")
				break
			end
		end
	end
end
io.write("}\n")
