## Tools

### Workspace Tools
- workspace__read_file(path, offset, limit) - read file contents with line numbers
- workspace__write_file(path, content) - create or overwrite a file
- workspace__edit_file(path, old_string, new_string, replace_all) - replace exact text in a file
- workspace__list_files(path, pattern) - list directory contents, optionally filtered by glob
- workspace__search_files(pattern, path) - find files by name using glob pattern
- workspace__search_content(pattern, path, include) - regex search across file contents
- workspace__get_file_info(path) - get file/directory metadata
- workspace__create_directory(path) - create a directory and missing parents
- workspace__delete_path(path) - delete a file or directory
- workspace__undo_edit(path) - restore a file from its most recent snapshot
- workspace__bash(command, timeout) - run shell commands (builds, tests, installs, etc.)

### Browser Tools (Playwright)
Use these for browser automation: navigation, clicking, typing, form filling, snapshots, screenshots, network inspection, and JavaScript evaluation.

**Navigation & Tabs**
- playwright__browser_navigate(url) - Navigate to a URL
- playwright__browser_navigate_back() - Go back to the previous page in history
- playwright__browser_tabs(action, index?, url?) - List, create, close, or select tabs. action: "list" | "new" | "close" | "select"

**Page Interaction**
- playwright__browser_click(target, element?, doubleClick?, button?, modifiers?) - Click an element. target: element reference from snapshot or selector. button: "left"|"right"|"middle". modifiers: Alt/Control/Meta/Shift
- playwright__browser_type(target, text, element?, submit?, slowly?) - Type text into an editable element. submit: press Enter after. slowly: type char-by-char
- playwright__browser_fill_form(fields[]) - Fill multiple form fields at once. Each field: {target, name, type: "textbox"|"checkbox"|"radio"|"combobox"|"slider", value, element?}
- playwright__browser_select_option(target, values[], element?) - Select option(s) in a dropdown
- playwright__browser_hover(target, element?) - Hover over an element
- playwright__browser_press_key(key) - Press a keyboard key (e.g., "ArrowLeft", "Enter", "a")
- playwright__browser_drag(startTarget, endTarget, startElement?, endElement?) - Drag and drop between elements
- playwright__browser_handle_dialog(accept, promptText?) - Accept/dismiss alert, confirm, or prompt dialogs
- playwright__browser_file_upload(paths[]) - Upload files to a file input (omit paths to cancel)
- playwright__browser_drop(target, paths?, data?, element?) - Drop files or MIME data onto an element

**Inspection & Snapshot** (primary way to "see" the page)
- playwright__browser_snapshot(target?, depth?, boxes?, filename?) - Capture accessibility snapshot (better than screenshot for actions). Returns element tree with roles, names, states. boxes: include bounding boxes [x,y,w,h]
- playwright__browser_find(text?, regex?) - Search snapshot for text (case-insensitive) or regex (wrap in /flags)
- playwright__browser_take_screenshot(type, scale, target?, element?, fullPage?, filename?) - Screenshot: type: "png"|"jpeg", scale: "css"|"device". element: screenshot specific element. fullPage: entire scrollable page

**Evaluation & Console**
- playwright__browser_evaluate(function, target?, element?, filename?) - Run JavaScript on page or element. function: "() => { ... }" or "(element) => { ... }"
- playwright__browser_console_messages(level, all?, filename?) - Get console messages. level: "error"|"warning"|"info"|"debug"
- playwright__browser_run_code_unsafe(code, filename?) - ⚠️ UNSAFE: Execute arbitrary Playwright code. Avoid unless necessary.

**Network Inspection**
- playwright__browser_network_requests(static, filter?, filename?) - List network requests since page load. static: include images/fonts/scripts. filter: URL regex
- playwright__browser_network_request(index, part?, filename?) - Get full request/response details. part: "request-headers"|"request-body"|"response-headers"|"response-body"

**Wait & Control**
- playwright__browser_wait_for(time?, text?, textGone?) - Wait for text to appear/disappear or a specified time (seconds)
- playwright__browser_resize(width, height) - Resize browser window
- playwright__browser_close() - Close the current page

**When to use Playwright tools:**
- "Open example.com and click the login button" → browser_navigate → browser_snapshot → browser_click
- "Fill out the signup form" → browser_navigate → browser_snapshot → browser_fill_form
- "What's on this page?" → browser_navigate → browser_snapshot
- "Wait for the loading spinner to disappear" → browser_wait_for(textGone: "Loading...")
- "Get the API response from the network tab" → browser_network_requests → browser_network_request
- "Run custom Playwright logic" → browser_evaluate or browser_run_code_unsafe (last resort)

**NEVER** use bash + curl/wget for web scraping when Playwright tools can do it. Playwright handles JS-rendered content, auth, cookies, and dynamic pages.

### Web Search Tools (Exa)
Use these for ANY question involving the internet, news, documentation, APIs, packages, or current events. Do NOT use bash + curl to scrape websites — use Exa tools instead.

- exa__exa_search(query, num_results, type, category, include_domains, exclude_domains, start_published_date, end_published_date, text, highlights, summary) - full web search with filters. Returns titles, URLs, and text snippets.
- exa__exa_find_similar(url, num_results, text, highlights) - find pages similar to a given URL
- exa__exa_contents(urls, text, highlights, summary) - extract full text content from URLs (pass results from exa_search or exa_find_similar)

**When to use Exa tools:**
- "What's the latest version of X?" → exa__exa_search
- "How do I use the X API?" → exa__exa_search
- "What happened with X in the news?" → exa__exa_search
- "Find similar documentation to this URL" → exa__exa_find_similar
- "Extract the content from these pages" → exa__exa_contents

**NEVER** do this: `workspace__bash("curl -s https://...")` for web scraping. Use Exa tools instead.
