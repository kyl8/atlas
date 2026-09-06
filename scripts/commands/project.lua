-- Project commands stay in Lua and use Atlas for storage and presentation.
local fs, ui = atlas.fs, atlas.ui
local store = "~/.atlas/data/projects.tsv"
local header = "atlas-projects-v1\n"
local reserved = { add=true, list=true, show=true, path=true, open=true, update=true,
    ["set-path"]=true, rename=true, remove=true, rm=true, check=true, help=true }

local function fail(message, code)
    error({ message=message, code=code or 1 }, 0)
end

local function must(value, message)
    if value == nil then fail(message or "Operation failed") end
    return value
end

local function name_key(value)
    return value:lower()
end

local function valid_text(value)
    return type(value) == "string" and value ~= "" and not value:find("[%z\1-\31\127]")
        and utf8.len(value) ~= nil
end

local function name(value)
    if not valid_text(value) or #value > 128 or value:find('[\\/:*?"<>|]')
        or value:match("^%s") or value:match("%s$") or value == "." or value == ".."
        or value:sub(1, 1) == "-" or reserved[name_key(value)] then
        fail("Invalid project name. Use a short name without path separators or a subcommand name.", 2)
    end
    return value
end

local function directory(input)
    if not valid_text(input) then fail("Provide a directory path.", 2) end
    local path = must(fs.resolve(input))
    if not must(fs.is_dir(path)) then fail("Project directory does not exist: " .. path, 2) end
    return path
end

local function hex(value)
    return (value:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

local function unhex(value)
    if #value % 2 ~= 0 or value:find("[^0-9a-f]") then fail("Invalid project database: " .. store) end
    return (value:gsub("..", function(pair) return string.char(tonumber(pair, 16)) end))
end

local function sorted(records, filter)
    local result = {}
    filter = filter and filter:lower()
    for _, record in pairs(records) do
        if not filter or record.name:lower():find(filter, 1, true)
            or record.path:lower():find(filter, 1, true) then
            result[#result + 1] = record
        end
    end
    table.sort(result, function(a, b) return name_key(a.name) < name_key(b.name) end)
    return result
end

local function read_store()
    if not must(fs.exists(store)) then return {}, "" end
    local source = must(fs.read(store))
    if source:sub(1, #header) ~= header or source:sub(-1) ~= "\n" then
        fail("Invalid project database; existing data was preserved: " .. store)
    end
    local records = {}
    for line in source:sub(#header + 1):gmatch("([^\n]*)\n") do
        local encoded_name, encoded_path, created, updated = line:match("^([0-9a-f]+)\t([0-9a-f]+)\t(%d+)\t(%d+)$")
        if not encoded_name then fail("Invalid record in " .. store .. "; existing data was preserved") end
        local record = { name=name(unhex(encoded_name)), path=unhex(encoded_path),
            created=tonumber(created), updated=tonumber(updated) }
        if not valid_text(record.path) or not math.tointeger(record.created)
            or not math.tointeger(record.updated) or records[name_key(record.name)] then
            fail("Invalid or duplicate record in " .. store)
        end
        records[name_key(record.name)] = record
    end
    return records, source
end

local function save(records, previous)
    local lines = { header }
    for _, record in ipairs(sorted(records)) do
        lines[#lines + 1] = hex(record.name) .. "\t" .. hex(record.path) .. "\t"
            .. string.format("%d\t%d\n", record.created, record.updated)
    end
    must(fs.mkdir("~/.atlas/data"))
    must(fs.write_atomic(store, table.concat(lines), previous))
end

local function lookup(records, key)
    local record = records[name_key(key)]
    if not record then fail("Unknown project '" .. key .. "'. Use atlas project list.", 2) end
    return record
end

local function same_path(a, b)
    return a:gsub("/", "\\"):gsub("\\+$", ""):lower()
        == b:gsub("/", "\\"):gsub("\\+$", ""):lower()
end

local function unique_path(records, path, except)
    for key, record in pairs(records) do
        if key ~= except and same_path(record.path, path) then
            fail("This directory is already registered as '" .. record.name .. "'.", 2)
        end
    end
end

local function json_string(value)
    return '"' .. value:gsub('[%z\1-\31\\"]', function(c)
        if c == '"' then return '\\"' end
        if c == '\\' then return '\\\\' end
        return string.format("\\u%04x", c:byte())
    end) .. '"'
end

local function json_record(record)
    local value = '{"name":' .. json_string(record.name) .. ',"path":' .. json_string(record.path)
        .. ',"created":' .. tostring(record.created) .. ',"updated":' .. tostring(record.updated)
    if record.exists ~= nil then value = value .. ',"exists":' .. tostring(record.exists) end
    return value .. '}'
end

local function display(records, title, check)
    if check then
        for _, record in ipairs(records) do record.exists = must(fs.is_dir(record.path)) end
    end
    if ui.json() then
        local result = {}
        for _, record in ipairs(records) do result[#result + 1] = json_record(record) end
        ui.data("[" .. table.concat(result, ",") .. "]\n")
        return
    end
    if ui.human() then ui.heading(title) end
    local rows = {}
    for _, record in ipairs(records) do
        local row = { { value=record.name, style="primary" }, { value=record.path, style="path" } }
        if check then row[3] = { value=record.exists and "OK" or "Missing",
            style=record.exists and "success" or "warning" } end
        rows[#rows + 1] = row
    end
    ui.table(check and { "PROJECT", "PATH", "STATUS" } or { "PROJECT", "PATH" }, rows)
    if #rows == 0 and ui.human() then ui.info('Register a project with: atlas project add "path"') end
end

local function done(action, record)
    if ui.json() then ui.data('{"action":' .. json_string(action) .. ',"project":' .. json_record(record) .. '}\n')
    else ui.success(action .. " '" .. record.name .. "'") end
end

local function count(args, minimum, maximum, usage)
    if #args < minimum or #args > maximum then fail("Usage: atlas project " .. usage, 2) end
end

local function execute(args)
    local new_shell = false
    local positional = {}
    for _, arg in ipairs(args) do
        if arg == "--go-session" then
            fail("The -gs flag needs the Atlas PowerShell integration.\n"
                .. "Load share/atlas/shell/atlas.ps1 from your Atlas installation into PowerShell.\n"
                .. "Then use: atlas project <name> -gs. Use -g to open another shell without integration.", 2)
        elseif arg == "--go" then
            if new_shell then fail("Usage: atlas project <name> -g", 2) end
            new_shell = true
        else
            positional[#positional + 1] = arg
        end
    end
    if new_shell then
        args = positional
        if not ((#args == 1 and not reserved[args[1]]) or (#args == 2 and args[1] == "open")) then
            fail("Usage: atlas project <name> -g", 2)
        end
    end
    local action = args[1] or "list"
    -- Validate the action before touching the project store.
    local known = reserved[action] ~= nil
    if action == "help" then fail("Use atlas project --help for commands and examples.", 2) end
    if action == "add" then
        count(args, 2, 4, 'add <path> [--name <name>]')
        if #args ~= 2 and (#args ~= 4 or args[3] ~= "--name") then
            fail('Usage: atlas project add <path> [--name <name>]', 2)
        end
    elseif action == "update" or action == "set-path" or action == "rename" then
        count(args, 3, 3, action .. " <name> <" .. (action == "rename" and "new-name" or "path") .. ">")
    elseif action == "list" or action == "check" then
        count(args, 0, 2, action .. " [filter]")
    elseif known then
        count(args, 2, 2, action .. " <name>")
    else
        count(args, 1, 1, "<name>")
    end

    local records, previous = read_store()
    if action == "list" or action == "check" then
        local selected = sorted(records, args[2])
        display(selected, "Projects", action == "check")
        if action == "check" then
            for _, record in ipairs(selected) do if not record.exists then return 1 end end
        end
        return 0
    end
    if action == "add" then
        local path = directory(args[2])
        local chosen = name(args[4] or path:gsub("[\\/]+$", ""):match("([^\\/]+)$"))
        local key = name_key(chosen)
        if records[key] then fail("Project '" .. chosen .. "' already exists. Use update or rename.", 2) end
        unique_path(records, path)
        local record = { name=chosen, path=path, created=os.time(), updated=os.time() }
        records[key] = record
        save(records, previous)
        done("Added", record)
        return 0
    end

    local record = lookup(records, known and args[2] or action)
    local key = name_key(record.name)
    if action == "remove" or action == "rm" then
        records[key] = nil
        save(records, previous)
        done("Removed registration for", record)
    elseif action == "rename" then
        local chosen = name(args[3])
        local replacement_key = name_key(chosen)
        if replacement_key ~= key and records[replacement_key] then fail("Project name already exists: " .. chosen, 2) end
        records[key] = nil
        record.name, record.updated = chosen, os.time()
        records[replacement_key] = record
        save(records, previous)
        done("Renamed to", record)
    elseif action == "update" or action == "set-path" then
        local path = directory(args[3])
        unique_path(records, path, key)
        record.path, record.updated = path, os.time()
        save(records, previous)
        done("Updated", record)
    elseif action == "path" then
        ui.data((ui.json() and json_string(record.path) or record.path) .. "\n")
    elseif action == "show" then
        display({record}, "Project", true)
    else
        if not must(fs.is_dir(record.path)) then
            fail("Directory is missing: " .. record.path .. "\nUse atlas project update " .. record.name .. ' "new path"')
        end
        if new_shell then
            -- Launch from the project directory without embedding its path in the command.
            local pid, reason = atlas.spawn("wt.exe", {"-w", "new", "new-tab", "-d", "."},
                {cwd=record.path})
            if not pid then
                fail("Could not launch Windows Terminal. Check that wt.exe is available in PATH.\n"
                    .. (reason or ""))
            end
            done("Opened Windows Terminal for", record)
        else
            must(atlas.open(record.path))
            done("Opened", record)
        end
    end
    return 0
end

local function handler(args)
    local ok, result = pcall(execute, args)
    if ok then return result end
    if type(result) == "table" and result.message then
        ui.error(result.message)
        return result.code
    end
    error(result, 0)
end

local help = {
    description = "Register, find, and open your project directories",
    aliases = { "p" },
    category = "Development",
    usage = "atlas project [<name>|list|add|update|rename|remove|show|path|check]",
    details = "With no arguments, lists projects. A project name opens its directory in File Explorer.\n"
        .. "Registrations live in ~/.atlas/data/projects.tsv. Remove only forgets a registration; project files stay untouched.",
    arguments = {
        { name="<name>", description="Open a registered project directly.", completion="projects" },
        { "add <path> [--name <name>]", "Register an existing directory; the folder name is used by default." },
        { "list [filter]", "List registrations alphabetically; optionally match part of a name or path." },
        { "open <name>", "Open the registered directory in Windows File Explorer." },
        { "update <name> <path>", "Change the saved directory (alias: set-path); does not move files." },
        { "rename <name> <new-name>", "Change the project shortcut; does not rename the directory." },
        { "remove <name>", "Forget the registration (alias: rm); never delete project files." },
        { "show <name>", "Show the saved name, full path, and whether the directory exists." },
        { "path <name>", "Print only the full path, even if the directory is currently unavailable." },
        { "check [filter]", "Check directory availability; exit with 1 if any selected path is missing." },
    },
    options = {
        { name="--go", short="-g", description="Open Windows Terminal in the project directory using its default profile; no Atlas shell integration required." },
        { name="-gs, --go-session", short="-G", description="Change the current PowerShell directory; requires the Atlas integration loaded from your profile." },
        { "--name <name>", "Choose the shortcut when adding; quote names containing spaces." },
        { "--json", "Output structured records, including created/updated Unix timestamps." },
        { "--plain", "Output complete names and paths as tab-separated data." },
        { "--quiet", "Suppress success messages; errors and requested data remain visible." },
    },
    examples = {
        { 'atlas project add "D:\\Dev\\Waluna"', "Register Waluna using its folder name." },
        { 'atlas project add . --name atlas', "Register the current directory as atlas." },
        { 'atlas project waluna', "Open waluna in File Explorer." },
        { 'atlas project waluna -g', "Open a new Windows Terminal window in waluna's directory." },
        { 'atlas project waluna -gs', "Change this PowerShell session to waluna's directory (shell integration)." },
        { 'atlas project update waluna "D:\\Work\\Waluna"', "Update a registration after moving the directory." },
        { 'atlas project rename waluna waluna-app', "Choose another shortcut." },
        { 'atlas project remove waluna-app', "Remove only the shortcut." },
        { 'Set-Location -LiteralPath (atlas project path atlas)', "Change this PowerShell session to the project directory." },
    },
    subcommands = {
        { name="list", description="List registered projects" },
        { name="add", description="Register a project directory", arguments={
            { name="path", completion="directory" }
        } },
        { name="open", description="Open a registered project", arguments={
            { name="project", completion="projects" }
        } },
        { name="update", description="Update a project's directory", aliases={"set-path"}, arguments={
            { name="project", completion="projects" }, { name="path", completion="directory" }
        } },
        { name="rename", description="Rename a project shortcut", arguments={
            { name="project", completion="projects" }
        } },
        { name="remove", description="Remove a project registration", aliases={"rm"}, arguments={
            { name="project", completion="projects" }
        } },
        { name="show", description="Show project details", arguments={{ name="project", completion="projects" }} },
        { name="path", description="Print a project's path", arguments={{ name="project", completion="projects" }} },
        { name="check", description="Check registered project directories" },
    },
}

atlas.completion.register("projects", function()
    local ok, records = pcall(read_store)
    if not ok then return {} end
    local result = {}
    for _, record in ipairs(sorted(records)) do
        result[#result + 1] = { value=record.name, description=record.path, kind="project" }
    end
    return result
end)

atlas.command("project", handler, help)
atlas.command("projects", function(args)
    local values = { "list" }
    for _, arg in ipairs(args) do values[#values + 1] = arg end
    return handler(values)
end, { description="List registered projects (shortcut for project list)", category="Development", usage="atlas projects [filter]",
    arguments={{name="filter", completion="projects"}},
    examples={{"atlas projects", "List all project shortcuts."}, {"atlas projects waluna", "Filter by name or path."}} })
