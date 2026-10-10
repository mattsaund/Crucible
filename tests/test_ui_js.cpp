// SPDX-License-Identifier: MIT
//
// The interface's text-to-markup functions.
//
// Markdown, syntax coloring and line diffs moved out of C++ and into
// ui/render.js when the window became a webview. They are the same functions
// doing the same job -- util/markdown.cpp, util/syntax.cpp and
// util/code_lines.cpp are gone -- so they are tested the same way, and the
// tests below are the old ones carried across.
//
// They run in JavaScriptCore, which is the engine WebKitGTK gives the webview
// on this platform, so this is the interpreter that will actually run them.
// The bytes are the embedded ones, not a copy: if the embed is stale, these
// fail.
#include <JavaScriptCore/JavaScript.h>

#include <cstring>
#include <string>
#include <vector>

#include "test_helpers.hpp"

namespace crucible::gui::web {
extern const unsigned char kRenderJs[];
extern const unsigned int  kRenderJs_size;
extern const unsigned char kPageHtml[];
extern const unsigned int  kPageHtml_size;
}  // namespace crucible::gui::web

namespace {

/// A JSStringRef that releases itself, because every call below makes one.
class Str {
public:
    explicit Str(const std::string& text) : ref_(JSStringCreateWithUTF8CString(text.c_str())) {}
    explicit Str(JSStringRef ref) : ref_(ref) {}
    ~Str() { if (ref_) JSStringRelease(ref_); }
    Str(const Str&)            = delete;
    Str& operator=(const Str&) = delete;
    operator JSStringRef() const { return ref_; }   // NOLINT(google-explicit-constructor)

    std::string utf8() const {
        const std::size_t  bytes = JSStringGetMaximumUTF8CStringSize(ref_);
        std::vector<char>  buffer(bytes);
        JSStringGetUTF8CString(ref_, buffer.data(), bytes);
        return std::string(buffer.data());
    }

private:
    JSStringRef ref_;
};

/// One context with render.js already evaluated in it.
///
/// Built once: the tables in that file are the same every time and parsing
/// them per assertion would be the slowest part of this suite.
class Engine {
public:
    Engine() : ctx_(JSGlobalContextCreate(nullptr)) {
        const std::string source(reinterpret_cast<const char*>(crucible::gui::web::kRenderJs),
                                 crucible::gui::web::kRenderJs_size);
        const Str   script(source);
        JSValueRef  thrown = nullptr;
        JSEvaluateScript(ctx_, script, nullptr, nullptr, 0, &thrown);
        if (thrown != nullptr) {
            loaded_error_ = to_text(thrown);
        }
    }
    ~Engine() { JSGlobalContextRelease(ctx_); }
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    const std::string& load_error() const { return loaded_error_; }

    /// Evaluate an expression and return it as a string. A thrown exception
    /// comes back as "threw: ..." so a broken call shows up as a failed
    /// comparison rather than as a crash.
    std::string eval(const std::string& expression) {
        const Str  script(expression);
        JSValueRef thrown = nullptr;
        JSValueRef value  = JSEvaluateScript(ctx_, script, nullptr, nullptr, 0, &thrown);
        if (thrown != nullptr) {
            return "threw: " + to_text(thrown);
        }
        return to_text(value);
    }

private:
    std::string to_text(JSValueRef value) const {
        JSValueRef thrown = nullptr;
        JSStringRef text  = JSValueToStringCopy(ctx_, value, &thrown);
        if (text == nullptr) {
            return "<unconvertible>";
        }
        const Str held(text);
        return held.utf8();
    }

    JSGlobalContextRef ctx_;
    std::string        loaded_error_;
};

Engine& js() {
    static Engine engine;
    return engine;
}

/// Every script in the page, in the order it loads them.
///
/// The page is one document by the time it is compiled in -- see
/// cmake/BundlePage.cmake -- so this is the error handler, then render.js,
/// then base.js and each view, then the one line that starts it all.
std::vector<std::string> page_scripts() {
    const std::string page(reinterpret_cast<const char*>(crucible::gui::web::kPageHtml),
                           crucible::gui::web::kPageHtml_size);
    std::vector<std::string> scripts;
    const std::string open = "<script>";
    for (std::size_t at = page.find(open); at != std::string::npos; at = page.find(open, at)) {
        const std::size_t start = at + open.size();
        const std::size_t close = page.find("</script>", start);
        if (close == std::string::npos) {
            break;
        }
        scripts.push_back(page.substr(start, close - start));
        at = close;
    }
    return scripts;
}

/// What stands in for a browser: enough of `window` and `document` for the
/// page's scripts to load, and nothing that draws.
///
/// The views are functions from the state to a string, on purpose, and that
/// is what makes this possible -- they can be called here, with a state made
/// up for the occasion, and what comes back can be read.
const char* const kNoBrowser = R"JS(
var window = this;
window.addEventListener = function () {};
var document = {
  addEventListener: function () {},
  getElementById: function () { return null; },
  querySelector: function () { return null; },
  activeElement: null,
  body: { getAttribute: function () { return ''; },
          classList: { add: function () {}, remove: function () {} } },
};
var localStorage = { getItem: function () { return null; }, setItem: function () {} };
var navigator = {};
function requestAnimationFrame() {}
function setTimeout() { return 0; }
function clearTimeout() {}
)JS";

/// A state with something of everything in it: a project, seats in every
/// phase, a turn of every kind, a cook that is running, an edit waiting.
const char* const kBusyState = R"JS(
state.config = {
  models_dir: '/models', system_prompt: 'Be exact.', reasoning_effort: 'medium',
  router: { model: 'router.gguf' },
  defaults: { temperature: 0.7, top_p: 0.9, top_k: 40, min_p: 0.05, repeat_penalty: 1.1,
              repeat_last_n: 64, max_tokens: 2048, n_ctx: 8192, n_batch: 512, n_threads: 0,
              n_gpu_layers: -1, split_mode: 'layer', flash_attn: true },
  experts: [{ id: 'physics', model: 'physics.gguf' },
            { id: 'claude', model: 'claude-opus-5-5', provider: 'anthropic' }],
  providers: [{ id: 'anthropic', kind: 'anthropic', has_key: true }],
  gpu: { mode: 'priority', priority: [1, 0], main_gpu: 0, gpu_only: true, vram_only: false },
  routing: { min_confidence: 0.6, default_expert: 'claude', keep_delegator_loaded: false },
  tools: { web_search: true, search_provider: 'brave', search_endpoint: '', search_api_key: 'k',
           search_results: 5, search_timeout: 10, search_rounds: 3, auto_edits: false,
           overflow: 'rolling', workshop_timeout: 120 },
  ui: { check_updates: true },
};
state.snapshot = {
  mood: 'loading', status: 'swapping in Physics', busy: true, delegator_ready: false,
  delegator_progress: 0.42, context_used: 7000, context_size: 8192, tokens_per_second: 31.5,
  session_usage: { input_tokens: 15300, output_tokens: 920 },
  project_usage: { input_tokens: 2500000, output_tokens: 81000 },
  notices: ['opened demo', '<b>not markup</b>'],
  linked: 'physics', resident: 'physics', version: '0.0.0', auto_edits: false,
  update: { latest: '9.9.9', page: 'https://example.test', command: 'crucible --update' },
  project: { open: true, root: '/home/me/work/demo', display: '~/work/demo', name: 'demo' },
  delegator: { model: 'router.gguf', stays_loaded: false },
  experts: [
    { id: 'math', name: 'Mathematics', tag: 'MATH', blurb: 'algebra', phase: 'dormant', progress: 0, model: 'math.gguf' },
    { id: 'physics', name: 'Physics', tag: 'PHYS', blurb: 'forces & "fields"', phase: 'loading', progress: 0.5, model: 'physics.gguf' },
    { id: 'claude', name: 'Claude', tag: 'CLD', blurb: 'everything else', phase: 'dormant', progress: 0,
      model: 'claude-opus-5-5', provider: 'Anthropic', default: true },
    { id: 'gone', name: 'Gone', tag: 'GONE', blurb: 'its file moved', phase: 'missing', progress: 0, model: 'gone.gguf' },
    { id: 'empty', name: 'Empty', tag: 'EMPT', blurb: 'no model', phase: 'unconfigured', progress: 0, model: '' },
  ],
  turns: [
    { prompt: 'why is the sky <blue>?', reply: 'Rayleigh **scattering**.\n\n```python\nprint(1)\n```',
      reasoning: 'short wavelengths scatter more', streaming: false, canceled: false, failed: false,
      tokens_per_second: 40.1, prompt_tokens: 1200, output_tokens: 56, load_ms: 2300,
      route: { expert: 'physics', confidence: 0.93, source: 'router model', detail: '' },
      actions: [{ summary: 'wrote sky.py  +1 -0', body: '+print(1)', language: 'sky.py' },
                { summary: 'searched "rayleigh" -- 3 results from wikipedia' }] },
    { prompt: 'and at sunset?', reply: 'Claude declined this request', streaming: false, canceled: false,
      failed: true, tokens_per_second: 0, prompt_tokens: 0, output_tokens: 0, load_ms: 0,
      route: { expert: 'claude', confidence: 0.4, source: 'fallback', detail: 'undecided' } },
    { prompt: 'stopped one', reply: 'half an', streaming: false, canceled: true, failed: false,
      tokens_per_second: 12, prompt_tokens: 10, output_tokens: 3, load_ms: 0 },
    { prompt: 'still going', reply: '', streaming: true, canceled: false, failed: false,
      tokens_per_second: 0, prompt_tokens: 0, output_tokens: 0, load_ms: 0 },
  ],
  pending_edit: { path: 'src/calc.py', before: 'a = 1\n', after: 'a = 2\n' },
  cook: { running: true, id: 'c1', goal: 'make the tests pass', state: 'asking',
          question: 'Which test runner?', outcome: '', headline: '', iterations: 3, started: 1,
          ended: 0, seconds: 754, experts: ['physics', 'math'], files: ['src/calc.py'],
          total: 3, shown_from: 1,
          steps: [{ iteration: 1, expert: 'physics', kind: 'read', summary: 'read src/calc.py', ok: true, ms: 4 },
                  { iteration: 2, expert: 'math', kind: 'write', summary: 'wrote src/calc.py', ok: true, ms: 9,
                    detail: '-a = 1\n+a = 2', changed: ['src/calc.py'] },
                  { iteration: 3, expert: 'math', kind: 'run', summary: 'pytest failed', ok: false, ms: 900,
                    detail: 'E assert 1 == 2' }] },
};
state.models = { directory: '/models', models: [
  { name: 'physics.gguf', path: '/models/physics.gguf', bytes: 1200000000 },
  { name: 'kitchen-physicist-Q4_K_M.gguf', path: '/models/kitchen-physicist-Q4_K_M.gguf', bytes: 900000000 }],
  display: '/models' };
state.providers = {
  providers: [{ id: 'anthropic', name: 'Anthropic', kind: 'anthropic', base_url: '', endpoint: 'https://api.anthropic.com',
                on_your_network: false, key: { source: 'convention', present: true, variable: 'ANTHROPIC_API_KEY' },
                models: ['claude-opus-5-5'], seats: ['Claude'] },
              { id: 'box', name: 'The box', kind: 'openai', base_url: 'http://192.168.1.9:8080/v1',
                endpoint: 'http://192.168.1.9:8080/v1', on_your_network: true,
                key: { source: 'none', present: false, variable: '' }, models: [], seats: [] }],
  known: [{ name: 'Anthropic', kind: 'anthropic', base_url: '', key_variable: 'ANTHROPIC_API_KEY', note: 'Claude.' },
          { name: 'DeepSeek', kind: 'openai', base_url: 'https://api.deepseek.com/v1', key_variable: 'DEEPSEEK_API_KEY', note: '' }] };
state.devices = { gpus: [{ index: 0, name: 'RTX 4070', backend: 'CUDA', memory_total: 12e9, memory_free: 11e9 },
                         { index: 1, name: 'RTX 3060', backend: 'CUDA', memory_total: 12e9, memory_free: 6e9 }],
                  support: { split: '', gpu_only: '', vram_only: 'This backend cannot tell dedicated memory from shared.' } };
state.runtimes = { loadable: true, directory: '/data/runtimes', bytes: 5e8, runtimes: [
  { id: 'cuda', name: 'CUDA', blurb: 'NVIDIA cards', installed: true, active: true, devices: 2, bytes: 5e8,
    stale: true, source: 'downloaded', llama_tag: 'b1', built_at: '2026-10-01', needs_tag: 'b2', needs_tool: 'nvcc',
    buildable: true, blocker: '', modules: [{ name: 'libggml-cuda.so', preferred: true }] },
  { id: 'vulkan', name: 'Vulkan', blurb: 'any GPU', installed: false, active: false, devices: 0, bytes: 0,
    stale: false, source: '', llama_tag: '', built_at: '', needs_tag: 'b2', needs_tool: 'glslc',
    buildable: false, blocker: 'glslc is not installed', modules: [] }] };
state.build = { phase: 'compiling', running: true, finished: false, percent: 0.4, step: 'ggml-cuda.cu',
                label: 'compiling 40%', error: '', log: ['[40%] Building'], log_file: '/data/build.log', backend: 'cuda' };
state.trainer = { ready: false, present: true, flavor: 'cuda', python: '3.12', torch: '2.6', installed_at: '2026-10-01',
                  bytes: 6e9, note: 'bitsandbytes will not import', directory: '/data/trainer',
                  usable_gpus: ['RTX 4070'], unusable_gpus: ['RTX 5060 Ti'] };
state.flavors = [{ id: 'cuda', note: 'NVIDIA', download: 3e9, installed: 7e9, suggested: true, steps: ['PyTorch (CUDA)'] },
                 { id: 'cpu', note: 'anything', download: 3e8, installed: 1e9, suggested: false, steps: ['PyTorch'] }];
state.install = { phase: 'failed', running: false, finished: true, percent: 0, step: '', label: 'failed',
                  error: 'pip exited 1', log: ['ERROR: no matching distribution'], log_file: '/data/install.log', flavor: 'cuda' };
state.about = { version: '0.0.0', update: { latest: '9.9.9', available: true, page: 'https://example.test',
                                            command: 'crucible --update', checked_at: 1, checks: true },
                files: { config: '/c/config.json', data: '/d', models: '/models', runtimes: '/d/runtimes',
                         projects: '/d/projects', log: '/d/crucible.log' },
                trusted: ['/home/me/work/demo'] };
state.history = { sessions: [{ id: 's1', title: 'why is the sky blue', when: 'today', turns: 2 }],
                  cooks: [{ id: 'c0', goal: 'tidy up', state: 'done', when: 'yesterday', files: 2, steps: 14, seconds: 3700 }] };
var aRecipe = function (stage, more) {
  return Object.assign({ id: 'kitchen-' + stage, name: 'Kitchen ' + stage, purpose: 'kitchen physics',
    base: { source: 'hub', id: 'unsloth/Llama-3.2-1B', label: 'Llama-3.2-1B' },
    data: [{ source: 'local', id: '/data/q.jsonl', label: 'q.jsonl', path: '/data/q.jsonl' }], tools: [],
    method: 'qlora', format: 'gguf', quantization: 'Q4_K_M', parameters_b: 1.2, epochs: 2, context: 512,
    learning_rate: 1e-5, trained_path: stage === 'draft' ? '' : '/lab/kitchen.gguf', trained_there: stage !== 'testing',
    trained_bytes: 9e8, stage: stage, stage_text: stage, started_at: 1, finished_at: 0,
    missing: stage === 'draft' ? ['data'] : [], export_bytes: 8e8,
    fit: { possible: false, needed: 9e9, have: 6e9, note: '', known: true } }, more || {});
};
state.recipes = { memory: 6e9, recipes: [aRecipe('draft'), aRecipe('training'), aRecipe('testing'), aRecipe('finished')] };
state.run = { phase: 'running', recipe: 'kitchen-training', name: 'Kitchen', label: 'training  step 12 of 40',
              percent: 0.3, step: 12, total: 40, loss: 1.8, curve: [2.4, 2.1, 1.9, 1.8], device: 'RTX 4070',
              records: 1200, trainable: 4200000, seconds_left: 600, seconds: 90, error: '', hint: '',
              notes: ['no converter here'], log: [], log_file: '', produced: '' };
)JS";

/// The whole page's scripts, loaded with no browser under them.
class Page {
public:
    Page() : ctx_(JSGlobalContextCreate(nullptr)) {
        run(kNoBrowser);
        const std::vector<std::string> scripts = page_scripts();
        // All but the last, which is the one line that starts the page and
        // so the one that would go looking for a document.
        for (std::size_t i = 0; i + 1 < scripts.size() && error_.empty(); ++i) {
            run(scripts[i]);
        }
    }
    ~Page() { JSGlobalContextRelease(ctx_); }
    Page(const Page&)            = delete;
    Page& operator=(const Page&) = delete;

    const std::string& load_error() const { return error_; }

    /// Evaluate and return as text; "threw: ..." when it threw.
    std::string eval(const std::string& expression) {
        const Str  script(expression);
        JSValueRef thrown = nullptr;
        JSValueRef value  = JSEvaluateScript(ctx_, script, nullptr, nullptr, 0, &thrown);
        return thrown != nullptr ? "threw: " + to_text(thrown) : to_text(value);
    }

private:
    void run(const std::string& source) {
        const std::string result = eval(source);
        if (result.rfind("threw: ", 0) == 0) {
            error_ = result;
        }
    }

    std::string to_text(JSValueRef value) const {
        JSValueRef  thrown = nullptr;
        JSStringRef text   = JSValueToStringCopy(ctx_, value, &thrown);
        if (text == nullptr) {
            return "<unconvertible>";
        }
        const Str held(text);
        return held.utf8();
    }

    JSGlobalContextRef ctx_;
    std::string        error_;
};

Page& page() {
    static Page loaded;
    return loaded;
}

/// Draw `expression` and say what is wrong with the markup, or "ok".
///
/// Not a validator. It checks the two mistakes a template literal makes
/// easily and a browser hides completely: an interpolation that came out as
/// the word "undefined", and a container that was opened and not closed --
/// which a browser repairs by swallowing whatever came after it.
std::string drawn(const std::string& expression) {
    return page().eval(
        "(function () { var html; try { html = " + expression + "; }"
        " catch (e) { return 'threw: ' + e + (e.stack ? ' @ ' + e.stack.split('\\n')[0] : ''); }"
        " if (typeof html !== 'string') return 'not a string: ' + typeof html;"
        " if (html.indexOf('undefined') >= 0) return 'says undefined near: '"
        "   + html.substr(Math.max(0, html.indexOf('undefined') - 80), 120);"
        " if (html.indexOf('NaN') >= 0) return 'says NaN near: '"
        "   + html.substr(Math.max(0, html.indexOf('NaN') - 80), 120);"
        // Parts of a line are set apart by space, not by a dot between them.
        " var dot = html.search(/\\s\\u00b7\\s/);"
        " if (dot >= 0) return 'a dot between words near: ' + html.substr(Math.max(0, dot - 60), 120);"
        " var tags = ['div', 'button', 'span', 'select', 'details', 'form', 'label', 'aside'];"
        " for (var i = 0; i < tags.length; i++) {"
        "   var opened = (html.match(new RegExp('<' + tags[i] + '[\\\\s>]', 'g')) || []).length;"
        "   var closed = (html.match(new RegExp('</' + tags[i] + '>', 'g')) || []).length;"
        "   if (opened !== closed) return tags[i] + ': ' + opened + ' opened, ' + closed + ' closed';"
        " }"
        " return 'ok'; })()");
}

/// Does `expression` evaluate to something containing `needle`?
bool has(const std::string& expression, const std::string& needle) {
    const std::string got = js().eval(expression);
    if (got.find(needle) == std::string::npos) {
        std::printf("      expression: %s\n      wanted:     %s\n      got:        %s\n",
                    expression.c_str(), needle.c_str(), got.c_str());
        return false;
    }
    return true;
}

/// A JS string literal, single-quoted, with the characters that would end it
/// escaped. The tests below are full of backslashes and quotes.
std::string lit(const std::string& text) {
    std::string out = "'";
    for (const char ch : text) {
        switch (ch) {
            case '\'': out += "\\'";  break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:   out += ch;     break;
        }
    }
    return out + "'";
}

std::string highlight(const std::string& code, const std::string& lang) {
    return "highlight(" + lit(code) + ", " + lit(lang) + ")";
}

}  // namespace

TEST(render_js_loads_in_javascriptcore) {
    CHECK(js().load_error().empty());
    if (!js().load_error().empty()) {
        std::printf("      %s\n", js().load_error().c_str());
    }
    CHECK_EQ(js().eval("typeof highlight"), "function");
    CHECK_EQ(js().eval("typeof markdown"), "function");
    CHECK_EQ(js().eval("typeof diffLines"), "function");
}

TEST(every_script_in_the_page_parses) {
    // Built, not run. Parsing is the half that fails silently: a syntax
    // error in one of the page's files reaches the user as a window with
    // nothing in it and nothing in the log.
    const std::vector<std::string> scripts = page_scripts();
    // The error handler, render.js, base.js, the shell, the attachments, the
    // views and the preview, and the line that starts them.
    CHECK(scripts.size() >= 12);
    for (const std::string& script : scripts) {
        const std::string expression =
            "(function(){ try { new Function(" + lit(script) + "); return 'parsed'; }"
            "catch (e) { return String(e); } })()";
        const std::string result = js().eval(expression);
        CHECK_EQ(result, "parsed");
        if (result != "parsed") {
            std::printf("      in the script that begins: %.70s\n", script.c_str());
        }
    }
}

// --- the views -----------------------------------------------------------
//
// Each is a function from the state to a string, so each can be called here
// with no window. What that catches is the kind of mistake that otherwise
// waits for somebody to open the one screen it is on.

TEST(the_page_loads_with_no_browser_under_it) {
    CHECK(page().load_error().empty());
    if (!page().load_error().empty()) {
        std::printf("      %s\n", page().load_error().c_str());
    }
    for (const char* view : {"chat", "build", "create", "history", "settings"}) {
        CHECK_EQ(page().eval(std::string("typeof views.") + view), "function");
    }
    // Cook is folded into Build: there is no view for it, and a cook's
    // journal is drawn by what draws a build's.
    CHECK_EQ(page().eval("typeof views.cook"), "undefined");
}

TEST(every_view_draws_before_anything_has_been_fetched) {
    // The state the page is in for its first frame: no snapshot worth the
    // name, no config, nothing fetched. Every view has to have something to
    // say in it, because every view can be the first one drawn.
    CHECK_EQ(drawn("topView()"), "ok");
    for (const char* view : {"chat", "build", "create", "history", "settings"}) {
        const std::string result = drawn(std::string("views.") + view + "()");
        CHECK_EQ(result, "ok");
    }
    for (const char* pane : {"agents", "code", "source", "preview"}) {
        CHECK_EQ(drawn(std::string("(state.buildPane = '") + pane + "', views.build())"), "ok");
    }
    // No project is no longer a wall: the box is open, because a conversation
    // with none open gets a scratch folder of its own -- and opening a project
    // is the right-hand panel's, not the top bar's.
    CHECK(page().eval("topView()").find("open-project") == std::string::npos);
    CHECK(page().eval("views.chat()").find("id=\"prompt\"") != std::string::npos);
}

TEST(every_view_draws_a_session_with_everything_in_it) {
    const std::string loaded = page().eval(kBusyState);
    CHECK(loaded.rfind("threw: ", 0) != 0);
    if (loaded.rfind("threw: ", 0) == 0) {
        std::printf("      %s\n", loaded.c_str());
    }
    CHECK_EQ(drawn("topView()"), "ok");
    CHECK_EQ(drawn("sideView()"), "ok");
    for (const char* view : {"chat", "build", "create", "history"}) {
        const std::string result = drawn(std::string("views.") + view + "()");
        CHECK_EQ(result, "ok");
    }
    for (const char* settings_page : {"general", "experts", "providers", "generation", "hardware",
                                      "runtimes", "training", "tools", "build", "about"}) {
        const std::string result = drawn(std::string("(state.settingsPage = '") + settings_page
                                         + "', views.settings())");
        CHECK_EQ(result, "ok");
        if (result != "ok") {
            std::printf("      on the %s page\n", settings_page);
        }
    }
    // One recipe at each stage, opened.
    for (const char* stage : {"draft", "training", "testing", "finished"}) {
        const std::string result = drawn(std::string("(state.open.recipe = 'kitchen-") + stage
                                         + "', views.create())");
        CHECK_EQ(result, "ok");
    }
    CHECK_EQ(drawn("(state.open.cook = state.snapshot.cook, views.history())"), "ok");
}

TEST(the_top_bar_says_which_folder_the_chat_works_in) {
    const std::string top = page().eval("topView()");
    // The path, but not a button: opening a project is the right-hand panel's.
    CHECK(top.find("class=\"project-path\"") != std::string::npos);
    CHECK(top.find("~/work/demo") != std::string::npos);
    // And opens it in the desktop's file browser; which folder is the window's to say.
    CHECK(top.find("data-act=\"show-folder\"") != std::string::npos);
    CHECK(top.find("open-project") == std::string::npos);
    CHECK(top.find("data-act=\"fold-right\"") != std::string::npos);
    // A new chat, before its first message makes its folder: where it will go.
    const std::string fresh = page().eval(
        "(() => { const was = state.snapshot.project;"
        " state.snapshot.project = { open: false, scratch_display: '~/.crucible/Scratchpad' };"
        " const out = topView(); state.snapshot.project = was; return out; })()");
    CHECK(fresh.find(">~/.crucible/Scratchpad<") != std::string::npos);
    // And that there is a newer version, on the gear.
    CHECK(top.find("9.9.9 is available") != std::string::npos);
}

TEST(an_unlabeled_block_is_colored_in_the_language_its_code_is_in) {
    // What gpt-oss sent for "a quick python script": the shebang above the
    // fence, and a fence that names no language. Both drew as gray text.
    const std::string drawn = page().eval(
        "markdown('#!/usr/bin/env python3\\n```\\ndef main():\\n    n = int(input())\\n"
        "    for _ in range(n):\\n        print(\"Hello World\")\\n\\n"
        "if __name__ == \"__main__\":\\n    main()\\n```')");
    CHECK(drawn.find(">python<") != std::string::npos);        // the block's header
    CHECK(drawn.find("#!/usr/bin/env python3") != std::string::npos);
    CHECK(drawn.find("<p>#!") == std::string::npos);            // not a paragraph above it
    CHECK(drawn.find("class=\"tok-") != std::string::npos);   // and colored
    // Each of these is plainly one language.
    const char* const cases[][2] = {
        {"fn main() {\\n    println!(\"hi\");\\n}", "rust"},
        {"#include <stdio.h>\\nint main(void) { return 0; }", "c++"},
        {"const x = 1;\\nconsole.log(x);", "javascript"},
        {"SELECT name FROM users;", "sql"},
        {"{\"a\": 1}", "json"},
    };
    for (const auto& one : cases) {
        const std::string out = page().eval(std::string("markdown('```\\n") + one[0] + "\\n```')");
        CHECK(out.find(std::string(">") + one[1] + "<") != std::string::npos);
        if (out.find(std::string(">") + one[1] + "<") == std::string::npos) {
            std::printf("      wanted %s for %s\n", one[1], one[0]);
        }
    }
    // And prose in a block stays text.
    CHECK(page().eval("markdown('```\\nThe answer is four.\\n```')").find(">text<") != std::string::npos);
}

TEST(what_crucible_is_fetching_is_shown_above_the_empty_chat) {
    const std::string drawn = page().eval(
        "(() => { const was = state.snapshot; state.snapshot = Object.assign({}, was, {turns: [], cook: null,"
        " setup: {running: true, items: ["
        "  {id: 'python', label: 'Python 3.12.15', state: 'done', detail: '', download: 66924371},"
        "  {id: 'runtime-cuda', label: 'CUDA runtime', state: 'working', detail: 'downloading',"
        "   progress: 0.4, download: 0},"
        "  {id: 'trainer', label: 'Training environment (cuda)', state: 'waiting', detail: '',"
        "   download: 3000000000}]}});"
        " const out = views.chat(); state.snapshot = was; return out; })()");
    CHECK(drawn.find("Setting up") != std::string::npos);
    CHECK(drawn.find("Python 3.12.15") != std::string::npos);
    CHECK(drawn.find("width:40%") != std::string::npos);        // measured, so a bar
    CHECK(drawn.find("Try again") == std::string::npos);        // nothing failed
    CHECK(drawn.find("Ask anything") != std::string::npos);     // and the box still works

    const std::string failed = page().eval(
        "(() => { const was = state.snapshot; state.snapshot = Object.assign({}, was, {turns: [], cook: null,"
        " setup: {running: false, items: ["
        "  {id: 'python', label: 'Python 3.12.15', state: 'failed',"
        "   detail: 'Python could not be downloaded: no route to host', download: 1}]}});"
        " const out = views.chat(); state.snapshot = was; return out; })()");
    CHECK(failed.find("no route to host") != std::string::npos);
    CHECK(failed.find("data-act=\"setup-retry\"") != std::string::npos);

    // Finished, it says nothing.
    CHECK(page().eval("(() => { const was = state.snapshot; state.snapshot = Object.assign({}, was,"
                      " {turns: [], cook: null, setup: {running: false, items: [{id: 'python', label: 'P',"
                      " state: 'done'}]}}); const out = views.chat(); state.snapshot = was;"
                      " return out; })()").find("Getting Crucible ready") == std::string::npos);
}

TEST(the_chat_is_the_conversation_and_nothing_the_program_says) {
    // Notices go to the side menu's status line -- its hover lists them --
    // and never into the transcript, however many there are.
    const std::string chat = page().eval("views.chat()");
    const std::size_t from = chat.find("id=\"transcript\"");
    const std::size_t to   = chat.find("id=\"jump\"");
    CHECK(from != std::string::npos && to != std::string::npos);
    const std::string transcript = chat.substr(from, to - from);
    CHECK(transcript.find("opened demo") == std::string::npos);
    CHECK(chat.find("class=\"notices\"") == std::string::npos);
    CHECK(page().eval("sideView()").find("opened demo") != std::string::npos);
}

TEST(a_delegator_that_would_not_load_says_so_on_its_own_row) {
    const std::string side = page().eval(
        "(() => { state.snapshot.delegator_problem = 'would put 11.8 GB on the 3060';"
        " const out = sideView(); delete state.snapshot.delegator_problem; return out; })()");
    CHECK(side.find("data-phase=\"missing\"") != std::string::npos);
    CHECK(side.find("would put 11.8 GB on the 3060") != std::string::npos);
    CHECK(side.find("Routing on keywords") != std::string::npos);
    const std::string chat = page().eval("views.chat()");
    CHECK(chat.substr(chat.find("id=\"transcript\"")).find("11.8 GB") == std::string::npos);
}

TEST(a_load_is_a_ring_with_the_figure_in_it_beside_the_name) {
    const std::string side = page().eval("sideView()");
    // The delegator, 42% loaded, and an expert at 50%.
    CHECK(side.find("class=\"ring\"") != std::string::npos);
    CHECK(side.find(">42%<") != std::string::npos);
    CHECK(side.find(">50%<") != std::string::npos);
    // A seat answered somewhere else is marked as one.
    CHECK(side.find("Answered by Anthropic") != std::string::npos);
    // Turned with the SVG's own transform, about the circle's own center.
    // A CSS rotation put the arc somewhere else in WebKit, half off the ring.
    CHECK(side.find("transform=\"rotate(-90 16 16)\"") != std::string::npos);
}

TEST(the_provider_dialog_asks_for_an_address_a_key_and_a_model) {
    const std::string modal = page().eval(
        "modalView({ kind: 'provider', id: '', name: '', preset: '', base_url: '', api_key: '',"
        " model: '', listed: [] })");
    CHECK(modal.find(">Address<") != std::string::npos);
    CHECK(modal.find("API key") != std::string::npos);
    CHECK(modal.find("id=\"pv-model\"") != std::string::npos);
    // A template is a dropdown now, and the wire format is not asked at all.
    CHECK(modal.find("id=\"pv-template\"") != std::string::npos);
    CHECK(modal.find("It speaks") == std::string::npos);
    CHECK(modal.find("class=\"chip") == std::string::npos);
}

TEST(hardware_has_no_output_card_choice_and_tools_no_edit_checkbox) {
    CHECK(page().eval("(state.settingsPage = 'hardware', views.settings())")
              .find("holds the output") == std::string::npos);
    CHECK(page().eval("(state.settingsPage = 'tools', views.settings())")
              .find("Apply edits without asking") == std::string::npos);
    // And the build's box has the same Auto button the chat's has.
    CHECK(page().eval("(state.buildPane = 'agents', views.build())").find("data-act=\"auto-edits\"") != std::string::npos);
}

TEST(the_box_draws_what_is_attached_as_tiles_under_a_plus_and_its_menu) {
    // With the cook out of the way, which would shut the chat's box.
    const char* const with_tiles =
        "(function () { var cook = state.snapshot.cook; state.snapshot.cook = null;"
        " state.attached.chat = ["
        "  { path: '/p/Resume.docx', name: 'Resume.docx', kind: 'document', label: 'DOCX', bytes: 5000 },"
        "  { path: '/p/sky.png', name: 'sky.png', kind: 'image', label: 'PNG',"
        "    thumb: 'data:image/jpeg;base64,AAAA', image: { mime: 'image/png', data: 'AAAA' } },"
        "  { path: '/p/r.pdf', name: 'r.pdf', kind: 'document', label: 'PDF', preview: 'Summary <b>x</b>' },"
        "  { path: '/p/src', name: 'src', kind: 'folder', label: 'FOLDER', files: 12 },"
        "  { path: '/p/x.bin', name: 'x.bin', kind: 'file', label: 'BIN', error: 'not text' },"
        "  { path: '/p/late.md', name: 'late.md', kind: 'file', label: 'MD', loading: true }];"
        " state.menu = 'attach:chat';"
        " var out = views.chat(); state.snapshot.cook = cook; return out; })()";
    CHECK_EQ(drawn(with_tiles), "ok");
    const std::string chat = page().eval(with_tiles);
    // The plus, open, with its two items.
    CHECK(chat.find("data-act=\"attach-menu\"") != std::string::npos);
    CHECK(chat.find("aria-expanded=\"true\"") != std::string::npos);
    CHECK(chat.find("Add files or photos") != std::string::npos);
    CHECK(chat.find("Ctrl+U") != std::string::npos);
    CHECK(chat.find("Add folder") != std::string::npos);
    // A tile of each kind, each with a way to take it out again.
    CHECK(chat.find(">DOCX<") != std::string::npos);
    CHECK(chat.find("class=\"tile picture") != std::string::npos);
    CHECK(chat.find("<img src=\"data:image/jpeg;base64,AAAA\"") != std::string::npos);
    CHECK(chat.find("class=\"page-text\">Summary &lt;b&gt;x&lt;/b&gt;") != std::string::npos);
    CHECK(chat.find("12 files") != std::string::npos);
    CHECK(chat.find("tile doc wrong") != std::string::npos);
    CHECK(chat.find("reading...") != std::string::npos);
    std::size_t removable = 0;
    for (std::size_t at = chat.find("data-act=\"attach-remove\""); at != std::string::npos;
         at = chat.find("data-act=\"attach-remove\"", at + 1)) {
        ++removable;
    }
    CHECK_EQ(removable, std::size_t{6});

    // Nothing is sent while one is still being read; then each goes as its
    // path, and the picture with the bytes the window shrank it to.
    CHECK_EQ(page().eval("String(attachmentsFor('chat'))"), "null");
    CHECK_EQ(page().eval("(state.attached.chat[5].loading = false,"
                         " JSON.stringify(attachmentsFor('chat').slice(0, 2)))"),
             "[{\"path\":\"/p/Resume.docx\"},{\"path\":\"/p/sky.png\",\"image\":{\"mime\":\"image/png\",\"data\":\"AAAA\"}}]");
    page().eval("(state.attached.chat = [], state.menu = null)");

    // The build keeps its own: what is in Chat's box is not in Build's.
    CHECK(page().eval("(state.buildPane = 'agents', views.build())").find("class=\"tiles\"") == std::string::npos);
}

TEST(a_sent_prompt_and_a_goal_show_what_was_attached) {
    const std::string chat = page().eval(
        "(function () { var t = state.snapshot.turns[0]; t.attachments = ["
        " { path: '/p/a.pdf', name: 'a <b>.pdf', label: 'PDF', kind: 'document' }];"
        " turnCache.clear(); var out = views.chat(); delete t.attachments; turnCache.clear(); return out; })()");
    CHECK(chat.find("class=\"attached\"") != std::string::npos);
    CHECK(chat.find("a &lt;b&gt;.pdf") != std::string::npos);
    const std::string cook = page().eval(
        "(function () { var c = state.snapshot.cook; c.attachments = ["
        " { path: '/p/src', name: 'src', label: 'FOLDER', kind: 'folder' }];"
        " state.buildPane = 'agents'; var out = views.build(); delete c.attachments; return out; })()");
    CHECK(cook.find(">FOLDER<") != std::string::npos);
}

TEST(the_drop_overlay_says_where_a_drop_goes_or_why_it_cannot) {
    // The busy state has a build running and asking. On the Build view that
    // shuts the box -- the answer goes in, and nothing else -- while the
    // chat's box takes a drop whatever the build is doing.
    const char* const on =
        "(function (view, what) { var was = state.view; state.view = view; var out = what();"
        " state.view = was; return out; })";
    CHECK_EQ(page().eval(std::string(on) + "('build', function () { return dropTarget().why; })"),
             "Answer the build's question first");
    CHECK(page().eval(std::string(on) + "('build', function () { return dropView(dropTarget()); })")
              .find("Answer the build&#39;s question first") != std::string::npos);
    CHECK_EQ(page().eval(std::string(on)
                         + "('build', function () { var s = state.snapshot.cook.state;"
                           " state.snapshot.cook.state = 'working'; var why = dropTarget().why;"
                           " state.snapshot.cook.state = s; return why; })"),
             "A build is running -- drop it on Chat, or wait for the build");
    CHECK_EQ(page().eval(std::string(on) + "('chat', function () { return dropTarget().mode; })"), "chat");
    const char* const without_cook =
        "(function (what) { var cook = state.snapshot.cook, view = state.view;"
        " state.snapshot.cook = null; var out = what();"
        " state.snapshot.cook = cook; state.view = view; return out; })";
    CHECK_EQ(page().eval(std::string(without_cook)
                         + "(function () { state.view = 'build'; return dropTarget().mode; })"),
             "build");
    const std::string from_history = page().eval(
        std::string(without_cook) + "(function () { state.view = 'history'; return dropView(dropTarget()); })");
    CHECK(from_history.find("Drop files or folders here") != std::string::npos);
    CHECK(from_history.find("They go in the box on Chat") != std::string::npos);
    // With no project open a drop is still taken: what it attaches goes with
    // a prompt, and the prompt goes in the Scratchpad.
    CHECK_EQ(page().eval(std::string(without_cook)
                         + "(function () { var open = state.snapshot.project.open;"
                           " state.snapshot.project.open = false; var mode = dropTarget().mode;"
                           " state.snapshot.project.open = open; return mode; })"),
             "chat");
}

TEST(a_drop_the_window_cannot_place_is_copied_in_pieces_and_attached) {
    // The road WebView2 and WKWebView take, with stand-ins for what they hand
    // the page: a folder with a file, a dependency folder, a hidden file and
    // a picture in it, and a file beside it.
    const std::string started = page().eval(R"JS(
      (function () {
        window.__calls = [];
        window.__keptCall = call; window.__keptRender = render;
        render = function () {};
        if (typeof btoa === 'undefined') {
          window.btoa = function (text) {
            var a = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/', out = '';
            for (var i = 0; i < text.length; i += 3) {
              var n = (text.charCodeAt(i) << 16) | ((text.charCodeAt(i + 1) || 0) << 8) | (text.charCodeAt(i + 2) || 0);
              out += a[(n >> 18) & 63] + a[(n >> 12) & 63]
                   + (i + 1 < text.length ? a[(n >> 6) & 63] : '=') + (i + 2 < text.length ? a[n & 63] : '=');
            }
            return out;
          };
        }
        call = function (method, params) {
          window.__calls.push([method, params]);
          if (method === 'attach.store') {
            return Promise.resolve({ path: '/kept/' + params.path, top: '/kept/' + params.path.split('/')[0] });
          }
          if (method === 'attach.inspect') {
            return Promise.resolve({ items: params.paths.map(function (p) {
              return { path: p, name: p.split('/').pop(), kind: p === '/kept/proj' ? 'folder' : 'text', label: 'X' };
            }) });
          }
          return Promise.resolve({});
        };
        function file(name, text) {
          var bytes = Array.from(text).map(function (c) { return c.charCodeAt(0); });
          return { isFile: true, isDirectory: false, name: name, file: function (ok) { ok({
            size: bytes.length,
            slice: function (a, b) { return { arrayBuffer: function () {
              return Promise.resolve(new Uint8Array(bytes.slice(a, b)).buffer); } }; } }); } };
        }
        function folder(name, children) {
          return { isFile: false, isDirectory: true, name: name, createReader: function () {
            var given = false;
            return { readEntries: function (ok) { var out = given ? [] : children; given = true; ok(out); } };
          } };
        }
        state.attached.chat = [];
        window.__done = false;
        keepDropped('chat', [
          folder('proj', [file('b.md', 'hello'), folder('node_modules', [file('x.js', 'no')]),
                          file('.env', 'secret'), file('logo.png', 'png'), file('a.txt', '')]),
          file('notes.txt', 'hi')
        ]).then(function () { window.__done = true; }, function (e) { window.__done = 'failed: ' + e; });
        return 'started';
      })())JS");
    CHECK_EQ(started, "started");
    CHECK_EQ(page().eval("String(window.__done)"), "true");
    // The folder is made first, then what is in it in name order -- without
    // node_modules, the hidden file or the picture -- and then the file.
    CHECK_EQ(page().eval(R"JS(JSON.stringify(window.__calls.filter(function (c) { return c[0] === 'attach.store'; })
                           .map(function (c) { return [c[1].path, c[1].data, !!c[1].folder, !!c[1].append]; })))JS"),
             R"([["proj","",true,false],["proj/a.txt","",false,false],["proj/b.md","aGVsbG8=",false,false],["notes.txt","aGk=",false,false]])");
    // Every piece of one drop goes to the same place.
    CHECK_EQ(page().eval("String(new Set(window.__calls.filter(function (c) { return c[0] === 'attach.store'; })"
                         ".map(function (c) { return c[1].batch; })).size)"),
             "1");
    // And what is attached is the copies, looked at like anything else.
    CHECK_EQ(page().eval("JSON.stringify(state.attached.chat.map(function (t) { return [t.path, t.kind, !!t.loading]; }))"),
             R"([["/kept/proj","folder",false],["/kept/notes.txt","text",false]])");
    page().eval("(call = window.__keptCall, render = window.__keptRender, state.attached.chat = [])");
}

TEST(an_allowed_write_stays_drawn_the_way_it_was_offered) {
    // A new file: the code itself, colored, numbered, on the green of an
    // addition -- not a gray unified diff with a header and a + on each line.
    const std::string created = page().eval(
        "writtenBlock('@@ line 1 @@\\n+def greet(name):\\n+    return name\\n', 'src/hello.py')");
    CHECK(created.find("code-add") != std::string::npos);
    CHECK(created.find(">python<") != std::string::npos);
    CHECK(created.find("@@") == std::string::npos);
    CHECK(created.find("class=\"kw\"") != std::string::npos || created.find("<span") != std::string::npos);

    // An edit: the lines that moved, matched up again, so the line between
    // two changes that neither touched reads as left alone.
    const std::string changed = page().eval(
        "writtenBlock('@@ line 1 @@\\n-def greet(name):\\n-\\n-if main:\\n-    print(1)\\n"
        "+def greet(name, n):\\n+\\n+if main:\\n+    print(2)\\n', 'hello.py')");
    CHECK(changed.find("from line 1") != std::string::npos);
    CHECK(changed.find("+2  \xE2\x88\x92" "2") != std::string::npos);
    CHECK(changed.find("dl dl-same") != std::string::npos);

    // In a turn: a write's action draws this, anything else its output.
    CHECK(page().eval("actionBody({ summary: 'created a.py', body: '@@ line 1 @@\\n+x = 1\\n', language: 'a.py' })")
              .find("code-add") != std::string::npos);
    CHECK(page().eval("actionBody({ summary: 'ran ls', body: 'a.py\\nb.py', language: '' })")
              .find("code-add") == std::string::npos);
}

TEST(the_line_beside_an_expert_says_how_it_was_routed_only_when_unusual) {
    CHECK(page().eval("views.chat()").find("router model") == std::string::npos);
    CHECK(page().eval("turnWho({ route: { expert: 'physics', confidence: 0.93, source: 'router model' }, load_ms: 2300 })")
              .find("93%") != std::string::npos);
    CHECK(page().eval("turnWho({ route: { expert: 'physics', confidence: 1, source: 'pinned' } })")
              .find(">pinned<") != std::string::npos);
    CHECK(page().eval("turnWho({ route: { expert: 'physics', confidence: 0.4, source: 'fallback' } })")
              .find(">fallback<") != std::string::npos);
}

TEST(with_no_project_the_box_is_open_and_nothing_is_said_about_it) {
    const std::string chat = page().eval(
        "(function () { var cook = state.snapshot.cook, project = state.snapshot.project,"
        " runtimes = state.runtimes, turns = state.snapshot.turns;"
        " state.snapshot.cook = null; state.snapshot.project = { open: false }; state.snapshot.turns = [];"
        " state.runtimes = { loadable: true, runtimes: [{ installed: true }] };"
        " var out = views.chat();"
        " state.snapshot.cook = cook; state.snapshot.project = project; state.runtimes = runtimes;"
        " state.snapshot.turns = turns; return out; })()");
    CHECK(chat.find("Ask anything") != std::string::npos);
    CHECK(chat.find("This chat gets a scratch folder") == std::string::npos);
    CHECK(chat.find("placeholder=\"Ask it something\"") != std::string::npos);
    CHECK(chat.find("id=\"prompt\" data-draft data-key=\"composer-key\" data-input=\"composer-grow\"") != std::string::npos);
    CHECK(chat.find("open a project to start") == std::string::npos);
}

TEST(the_right_panel_lists_recent_chats_across_projects_and_the_projects) {
    const std::string side = page().eval(
        "(function () { var w = state.recentsWidth, r = state.recents, session = state.snapshot.session;"
        " state.recentsWidth = 16; state.snapshot.session = '20261005-101500';"
        " state.recents = { chats: ["
        "   { id: '20261005-101500', title: 'why <b>orbits</b> decay', when: '1 hour ago', turns: 3,"
        "     project: '/home/me/orbit', project_name: 'orbit' },"
        "   { id: '20261004-090000', title: 'draft a note', when: 'yesterday', turns: 1,"
        "     project: '/home/me/.crucible/Scratchpad', project_name: 'Scratchpad' }],"
        "   projects: [{ root: '/home/me/orbit', name: 'orbit', display: '~/orbit', current: true },"
        "              { root: '/home/me/.crucible/Scratchpad', name: 'Scratchpad', display: '~/.crucible/Scratchpad' }] };"
        " var out = recentsView();"
        " state.recentsWidth = w; state.recents = r; state.snapshot.session = session; return out; })()");
    CHECK(side.find("RECENT CHATS") != std::string::npos);
    CHECK(side.find("New chat") != std::string::npos);
    CHECK(side.find("Open project") != std::string::npos);
    CHECK(side.find("class=\"action small\" data-act=\"new-chat\"") != std::string::npos);
    CHECK(side.find("why &lt;b&gt;orbits&lt;/b&gt; decay") != std::string::npos);
    CHECK(side.find("data-act=\"recent-chat\" data-id=\"20261004-090000\" data-project=\"/home/me/.crucible/Scratchpad\"") != std::string::npos);
    CHECK(side.find("class=\"recent here\"") != std::string::npos);
    CHECK(side.find("data-act=\"recent-project\" data-path=\"/home/me/orbit\"") != std::string::npos);
    // Shut, it is not drawn at all; the top bar has the way back.
    CHECK_EQ(page().eval("(function () { var w = state.recentsWidth; state.recentsWidth = 0;"
                         " var out = recentsView(); state.recentsWidth = w; return out; })()"), "");
    CHECK(page().eval("topView()").find("data-act=\"fold-right\"") != std::string::npos);
}

TEST(each_recent_chat_and_project_has_a_bin_but_not_while_something_runs) {
    const char* const draw =
        "(function (busy) { var w = state.recentsWidth, r = state.recents, b = state.snapshot.busy;"
        " state.recentsWidth = 16; state.snapshot.busy = busy;"
        " state.recents = { chats: ["
        "   { id: '20261005-101500', title: 'orbits', when: '1 hour ago', turns: 3,"
        "     project: '/home/me/orbit', project_name: 'orbit' },"
        "   { id: '20261004-090000', title: 'a note', when: 'yesterday', turns: 1, scratch: true,"
        "     project: '/home/me/.crucible/Scratchpad/20261004-090000', project_name: 'Scratchpad' }],"
        "   projects: [{ root: '/home/me/orbit', name: 'orbit', display: '~/orbit' }] };"
        " var out = recentsView();"
        " state.recentsWidth = w; state.recents = r; state.snapshot.busy = b; return out; })";
    const std::string idle = page().eval(std::string(draw) + "(false)");
    CHECK(idle.find("data-act=\"chat-delete\" data-id=\"20261005-101500\"") != std::string::npos);
    CHECK(idle.find("data-scratch=\"1\"") != std::string::npos);    // its folder goes too
    CHECK(idle.find("data-act=\"project-forget\" data-path=\"/home/me/orbit\"") != std::string::npos);
    CHECK(idle.find("class=\"r-row\"") != std::string::npos);
    const std::string busy = page().eval(std::string(draw) + "(true)");
    CHECK(busy.find("r-trash") == std::string::npos);
    CHECK(busy.find("data-act=\"recent-chat\"") != std::string::npos);
}

TEST(the_right_panel_ends_with_the_models_at_work) {
    // Local models above, the frontier model in use below: what each holds,
    // what it has cost, and how much of its provider's limits is used.
    const char* const draw =
        "(function (snap) { var was = {}; for (var k in snap) { was[k] = state.snapshot[k]; state.snapshot[k] = snap[k]; }"
        " var w = state.recentsWidth; state.recentsWidth = 16; var out = recentsView(); state.recentsWidth = w;"
        " for (var k2 in snap) state.snapshot[k2] = was[k2]; return out; })";
    const std::string busy = std::string(draw) + "({"
        " memory_total: 25769803776,"
        " local_models: [{ seat: '', file: 'router.gguf', bytes: 1073741824, delegator: true },"
        "                { seat: 'math', file: 'math.gguf', bytes: 9663676416, delegator: false }],"
        " frontier: [{ provider: 'Anthropic', model: 'claude-opus-5-5', last: 2, prompt: 50000, context: 200000,"
        "   session: { input: 1000, cache_read: 40000, cache_write: 0, output: 900, requests: 3, cost: 0.4213 },"
        "   month: { input: 9000, cache_read: 90000, cache_write: 0, output: 9000, requests: 30, cost: 12.3 },"
        "   limits: [{ what: 'requests', limit: 50, remaining: 4, resets: '6m0s' }] },"
        "  { provider: 'OpenAI', model: 'gpt-5', last: 1, prompt: 0, context: 0,"
        "   session: { input: 10, cache_read: 0, cache_write: 0, output: 5, requests: 1 },"
        "   month: { input: 10, cache_read: 0, cache_write: 0, output: 5, requests: 1 }, limits: [] }] })";
    CHECK_EQ(drawn(busy), "ok");
    const std::string panel = page().eval(busy);
    const std::size_t local = panel.find(">LOCAL<");
    const std::size_t frontier = panel.find(">FRONTIER<");
    CHECK(local != std::string::npos);
    CHECK(frontier != std::string::npos);
    CHECK(local < frontier);
    CHECK(panel.find("Delegator") != std::string::npos);
    CHECK(panel.find("Mathematics") != std::string::npos);
    CHECK(panel.find("9.0 GB") != std::string::npos);
    CHECK(panel.find("claude-opus-5-5") != std::string::npos);
    CHECK(panel.find("$0.42") != std::string::npos);
    CHECK(panel.find("this month") != std::string::npos);
    CHECK(panel.find("$12") != std::string::npos);
    CHECK(panel.find("4 of 50 left") != std::string::npos);
    CHECK(panel.find("u-meter full") != std::string::npos);    // 46 of 50 used
    CHECK(panel.find("25%") != std::string::npos);             // the context
    CHECK(panel.find("gpt-5") != std::string::npos);
    CHECK(panel.find("15 tokens") != std::string::npos);       // no price for it

    // Nothing loaded and nothing asked: it says so, and offers a provider.
    const std::string empty = std::string(draw) + "({ local_models: [], frontier: [], experts: [] })";
    CHECK_EQ(drawn(empty), "ok");
    const std::string none = page().eval(empty);
    CHECK(none.find("None loaded") != std::string::npos);
    CHECK(none.find("data-page=\"providers\"") != std::string::npos);
}

TEST(a_turns_thinking_starts_shut_and_stays_the_way_it_was_left) {
    const char* const thought =
        "(function () { var t = turnsView(state.snapshot.turns, 0);"
        " return t.slice(t.indexOf('<details class=\"work thought\"'), t.indexOf('<summary>thinking')); })()";
    CHECK(page().eval(thought).find(" open") == std::string::npos);
    page().eval("thoughtsOpen.add(thoughtKey(0))");
    CHECK(page().eval(thought).find(" open") != std::string::npos);
    page().eval("thoughtsOpen.clear()");
    // And there is no setting for it any more.
    CHECK(page().eval("(function () { state.settingsPage = 'generation'; return views.settings(); })()")
              .find("show_reasoning") == std::string::npos);
}

TEST(general_settings_start_with_the_models_folder) {
    const std::string drawn =
        page().eval("(function () { state.settingsPage = 'general'; return views.settings(); })()");
    const std::string general = drawn.substr(drawn.find("class=\"settings-page\""));
    CHECK(general.find("<h2>MODELS</h2>") != std::string::npos);
    CHECK(general.find("<h2>MODELS</h2>") < general.find("DELEGATOR</h2>"));
}

TEST(an_mlx_model_is_offered_to_an_expert_and_not_to_the_delegator) {
    const char* const with_models =
        "(function (what) { var m = state.models; state.models = { directory: '/m', display: '/m',"
        "  models: [{ name: 'small.gguf', bytes: 1000, format: 'gguf' },"
        "           { name: 'mlx-community/Qwen3-4B-4bit', bytes: 2000, format: 'mlx' }] %s };"
        "  var out = what(); state.models = m; return out; })";
    const auto with = [&](const std::string& extra, const std::string& expression) {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer), with_models, extra.c_str());
        return page().eval(std::string(buffer) + "(function () { return " + expression + "; })");
    };
    const std::string expert = with("", "modelSelect('', '', {})");
    CHECK(expert.find("mlx-community/Qwen3-4B-4bit (MLX, ") != std::string::npos);
    CHECK(expert.find("cannot run here") == std::string::npos);
    const std::string delegator = with("", "modelSelect('', '', {}, true)");
    CHECK(delegator.find("Qwen3-4B-4bit") == std::string::npos);
    CHECK(delegator.find("small.gguf") != std::string::npos);
    // Where MLX cannot run, it is listed -- so it is plain it was found --
    // and not offered, with the reason on it.
    const std::string here = with(", mlx_unavailable: 'MLX does not run on Windows'", "modelSelect('', '', {})");
    CHECK(here.find("cannot run here") != std::string::npos);
    CHECK(here.find(" disabled") != std::string::npos);
    CHECK(here.find("MLX does not run on Windows") != std::string::npos);
}

TEST(a_load_that_does_not_say_how_far_along_it_is_turns_rather_than_counts) {
    const std::string spinning = page().eval("ring(-1)");
    CHECK(spinning.find("animateTransform") != std::string::npos);
    CHECK(spinning.find("%") == std::string::npos);
    CHECK(page().eval("ring(0.5)").find(">50%<") != std::string::npos);
}

TEST(the_box_has_who_answers_how_hard_it_thinks_auto_and_an_arrow) {
    const char* const open_box =
        "(function (menu) { var cook = state.snapshot.cook, busy = state.snapshot.busy, m = state.menu;"
        " state.snapshot.cook = null; state.snapshot.busy = false; state.menu = menu;"
        " state.snapshot.reasoning_effort = 'high';"
        " var out = views.chat(); state.snapshot.cook = cook; state.snapshot.busy = busy; state.menu = m;"
        " return out; })";
    const std::string shut = page().eval(std::string(open_box) + "(null)");
    CHECK(shut.find("data-act=\"route-menu\"") != std::string::npos);
    CHECK(shut.find(">Delegator<") != std::string::npos);
    CHECK(shut.find(">High effort<") != std::string::npos);
    CHECK(shut.find(">Auto off<") != std::string::npos);
    CHECK(shut.find("class=\"send\"") != std::string::npos);
    // No hover text on Send, Auto or the effort menu: what each is, is on it.
    CHECK(shut.find("class=\"send\" aria-label=\"Send\" >") != std::string::npos
          || shut.find("class=\"send\" aria-label=\"Send\"") != std::string::npos);
    CHECK(shut.find("(Enter)") == std::string::npos);
    CHECK(shut.find("Edits apply as they are made") == std::string::npos);
    CHECK(shut.find("How hard a reasoning model thinks") == std::string::npos);
    CHECK(shut.find(">Send<") == std::string::npos);

    const std::string route = page().eval(std::string(open_box) + "('route:chat')");
    CHECK(route.find(">Delegator<") != std::string::npos);
    CHECK(route.find("mi-hint") == std::string::npos);
    CHECK(route.find("data-act=\"route-pick\"") != std::string::npos);
    CHECK(route.find("data-value=\"physics\"") != std::string::npos);
    const std::string effort = page().eval(std::string(open_box) + "('effort:chat')");
    CHECK(effort.find("REASONING EFFORT") != std::string::npos);
    CHECK(effort.find("Reasoning models only") != std::string::npos);
    CHECK(effort.find("mi-hint") == std::string::npos);
    CHECK(effort.find("data-act=\"effort-pick\"") != std::string::npos);
    CHECK(effort.find("data-value=\"low\"") != std::string::npos);

    // A seat chosen in the menu is where a prompt goes; one that has lost its
    // model sends prompts back to the delegator.
    CHECK_EQ(page().eval("(state.route = 'physics', routeTarget())"), "physics");
    CHECK_EQ(page().eval("(state.route = 'nobody', routeTarget())"), "");
    page().eval("(state.route = '')");
    CHECK(page().eval("(state.snapshot.auto_edits = true, views.chat())").find(">Auto on<") != std::string::npos);
    page().eval("(state.snapshot.auto_edits = false)");
}

TEST(what_came_from_a_model_is_never_markup) {
    // A notice, a prompt and a blurb, each with markup in it.
    const std::string chat = page().eval("views.chat()");
    CHECK(chat.find("<b>not markup</b>") == std::string::npos);
    CHECK(chat.find("&lt;b&gt;not markup&lt;/b&gt;") != std::string::npos);
    CHECK(chat.find("sky &lt;blue&gt;") != std::string::npos);
    CHECK(page().eval("sideView()").find("&quot;fields&quot;") != std::string::npos);
}

TEST(every_dialog_draws) {
    for (const char* modal : {
             "{ kind: 'trust', path: '/home/me/x' }",
             "{ kind: 'confirm', title: 'Sure?', body: 'It goes.', yes: 'Yes', no: 'No' }",
             "{ kind: 'new-expert', name: 'Rust', description: 'async', model: 'physics.gguf', provider: '' }",
             "{ kind: 'new-expert', name: 'C', description: 'd', model: 'claude-opus-5-5', provider: 'anthropic', error: 'taken' }",
             "{ kind: 'provider', id: '', name: '', preset: 'DeepSeek', base_url: 'https://api.deepseek.com/v1', api_key: '', model: '', listed: [] }",
             "{ kind: 'provider', id: 'anthropic', name: 'Anthropic', preset: '', base_url: 'https://api.anthropic.com', api_key: '', model: 'claude-opus-5-5', listed: ['claude-opus-5-5', 'claude-haiku-4-5'], has_key: true, note: 'listed' }",
             "{ kind: 'browser', wanted: { folder: true, title: 'Choose' }, path: '/home', "
             "  listing: { path: '/home', parent: '/', home: '/home/me', entries: ['me'], files: [{ name: 'a.gguf', bytes: 5 }] } }",
             "{ kind: 'browser', wanted: { title: 'Choose' }, path: '', listing: null }",
             "{ kind: 'tester', id: 'kitchen-testing', name: 'Kitchen', file: '/lab/k.gguf', seat: 'lab-test', from: 1 }",
         }) {
        const std::string result = drawn(std::string("modalView(") + modal + ")");
        CHECK_EQ(result, "ok");
        if (result != "ok") {
            std::printf("      for %s\n", modal);
        }
    }
    // The wizard, at each of its six steps, on a recipe with something chosen
    // and on an empty one.
    for (int step = 0; step < 6; ++step) {
        for (const char* recipe : {"aRecipe('draft')", "blankRecipe()"}) {
            const std::string hub = "{ query: 'q', items: [{ id: 'a/b', downloads: 1500, parameters_b: 1.2, gated: true }],"
                                    " searching: false, asked: true, error: '' }";
            const std::string result = drawn(
                "modalView({ kind: 'wizard', step: " + std::to_string(step) + ", recipe: " + recipe
                + ", hub: " + hub + ", dataHub: " + hub + ", toolHub: " + hub
                + ", fit: { methods: { qlora: { known: true, possible: true, needed: 4e9, have: 6e9 },"
                  " lora: { known: true, possible: false, needed: 9e9, have: 6e9 } },"
                  " sizes: { Q4_K_M: 8e8, Q8_0: 13e8 }, memory: 6e9 } })");
            CHECK_EQ(result, "ok");
            if (result != "ok") {
                std::printf("      at step %d of %s\n", step, recipe);
            }
        }
    }
}

// --- escaping ---------------------------------------------------------

TEST(escape_closes_the_tags_that_would_otherwise_open) {
    CHECK_EQ(js().eval("escape('<b>&</b>')"), "&lt;b&gt;&amp;&lt;/b&gt;");
    CHECK_EQ(js().eval("escape(null)"), "");
    CHECK_EQ(js().eval("escape(undefined)"), "");
    CHECK_EQ(js().eval("escape(7)"), "7");
}

// --- markdown ---------------------------------------------------------
    //
    // The block set util/markdown.hpp had, asked of the renderer that replaced
    // it.

TEST(headings_lists_and_rules_are_recognized) {
    const std::string source =
        "## What a pointer is\n\n"
        "- a memory address\n- a reference tool\n\n"
        "1. first\n2) second\n\n"
        "> quoted\n---\nordinary prose\n";
    CHECK(has("markdown(" + lit(source) + ")", "<h2>What a pointer is</h2>"));
    CHECK(has("markdown(" + lit(source) + ")", "<ul><li>a memory address</li>"
                                               "<li>a reference tool</li></ul>"));
    // Both ways a model numbers a list, and they are one list either way.
    CHECK(has("markdown(" + lit(source) + ")", "<ol><li>first</li><li>second</li></ol>"));
    CHECK(has("markdown(" + lit(source) + ")", "<blockquote>"));
    CHECK(has("markdown(" + lit(source) + ")", "<hr>"));
    CHECK(has("markdown(" + lit(source) + ")", "<p>ordinary prose</p>"));
}

TEST(a_table_is_recognized_by_the_row_under_its_header) {
    const std::string source =
        "| Planet | Radius (km) | Moons |\n"
        "|--------|------------:|:-----:|\n"
        "| Mercury | 2,440 | 0 |\n"
        "| Mars | 3,390 | 2 |\n";
    const std::string call = "markdown(" + lit(source) + ")";
    CHECK(has(call, "<table>"));
    CHECK(has(call, "<th>Planet</th>"));
    // The alignment the delimiter row asked for: right for the radius,
    // centered for the moons, and nothing said about the name.
    CHECK(has(call, "<th style=\"text-align:right\">Radius (km)</th>"));
    CHECK(has(call, "<th style=\"text-align:center\">Moons</th>"));
    // A thousands separator is not markup.
    CHECK(has(call, "<td style=\"text-align:right\">2,440</td>"));
    CHECK(has(call, "<td>Mars</td>"));
    CHECK_EQ(js().eval(call + ".match(/<tr>/g).length"), "3");
}

TEST(a_line_with_a_pipe_in_it_is_not_a_table) {
    // The failure this guards: an answer about shell pipelines is full of
    // pipes, and turning one into a one-row table would be worse than leaving
    // the pipes alone. What makes a table is the delimiter row under it.
    for (const char* prose : {"run `ls | grep foo` to filter",
                              "the options are a | b | c",
                              "| not | a | table |"}) {
        CHECK_EQ(js().eval("markdown(" + lit(prose) + ").includes('<table>')"), "false");
    }
}

TEST(a_table_without_outer_pipes_is_still_a_table) {
    // GitHub-flavored markdown allows them to be left off, and models do.
    const std::string call = "markdown('a | b\\n--- | ---\\n1 | 2\\n')";
    CHECK(has(call, "<table>"));
    CHECK(has(call, "<th>a</th>"));
    CHECK(has(call, "<td>2</td>"));
}

TEST(a_table_ends_where_its_rows_do) {
    CHECK(has("markdown('| a | b |\\n|---|---|\\n| 1 | 2 |\\nback to prose\\n')",
              "</table><p>back to prose</p>"));
}

TEST(a_code_block_says_what_it_is_and_numbers_its_lines) {
    // The header and the gutter are what make a block readable: a model that
    // says "line 12" is talking about something that has to be findable.
    const std::string call = "markdown('```python\\na = 1\\nb = 2\\n```')";
    CHECK(has(call, "<span class=\"lang\">python</span>"));
    CHECK(has(call, "2 lines"));
    CHECK(has(call, "code-gutter"));
    // The gutter counts the lines of code, not the fence.
    CHECK_EQ(js().eval(call + ".match(/code-gutter[^>]*>([^<]*)</)[1]"), "1\n2");
    CHECK(has(call, "data-act=\"copy\""));
}

TEST(one_line_of_code_is_one_line) {
    CHECK(has("markdown('```\\nonly\\n```')", "1 line<"));
}

TEST(a_fenced_block_is_code_all_the_way_to_its_close) {
    const std::string source =
        "Here:\n```python\n# not a heading\n- not a bullet\n**not bold**\n```\ndone\n";
    const std::string call = "markdown(" + lit(source) + ")";
    // Nothing inside a fence is markup, which is the point of a fence.
    CHECK_EQ(js().eval(call + ".includes('<h1>')"), "false");
    CHECK_EQ(js().eval(call + ".includes('<li>')"), "false");
    CHECK_EQ(js().eval(call + ".includes('<strong>')"), "false");
    CHECK(has(call, "<p>Here:</p>"));
    CHECK(has(call, "<p>done</p>"));
    CHECK(has(call, "python"));
}

TEST(code_keeps_its_indentation) {
    // Re-flowed code is code that no longer runs. (An unlabeled block of
    // Python is colored as Python now, so the keyword is in a span.)
    CHECK(has("markdown('```\\ndef f():\\n    return 1\\n```\\n')", "\n    <span class=\"tok-key\">return</span>"));
    CHECK(has("markdown('```text\\nfirst\\n    indented\\n```\\n')", "\n    indented"));
}

TEST(inline_styling_is_split_into_runs) {
    const std::string call = "markdown('plain **bold** and `code` and *italic*')";
    CHECK(has(call, "<strong>bold</strong>"));
    CHECK(has(call, "<code>code</code>"));
    CHECK(has(call, "<em>italic</em>"));
    // And the markers themselves are gone.
    CHECK_EQ(js().eval(call + ".replace(/<[^>]*>/g,'')"), "plain bold and code and italic");
}

TEST(a_lone_asterisk_is_not_the_start_of_anything) {
    // An expert writing "3 * 4" or a footnote marker must not turn the rest of
    // the line italic and lose the character while doing it.
    for (const char* line : {"3 * 4 = 12", "see note *", "a_b_c and snake_case"}) {
        const std::string call = "markdown(" + lit(line) + ")";
        CHECK_EQ(js().eval(call + ".includes('<em>')"), "false");
        CHECK_EQ(js().eval(call + ".replace(/<[^>]*>/g,'')"), line);
    }
}

TEST(inline_code_is_not_searched_for_markup) {
    const std::string call = "markdown('use `a ** b` for powers')";
    CHECK_EQ(js().eval(call + ".includes('<strong>')"), "false");
    CHECK_EQ(js().eval(call + ".replace(/<[^>]*>/g,'')"), "use a ** b for powers");
}

TEST(text_with_no_markdown_in_it_survives_unchanged) {
    // The regression that matters: a model that writes plain prose must come
    // out exactly as it went in.
    const std::string plain = "Water boils at 100 C. That is 212 F, at sea level.";
    CHECK_EQ(js().eval("markdown(" + lit(plain) + ")"), "<p>" + plain + "</p>");
}

TEST(markdown_never_lets_a_tag_through_from_the_model) {
    // A reply is text that chose some emphasis, not a document that brings
    // its own HTML.
    CHECK(has("markdown('a <script>x</script> b')", "&lt;script&gt;"));
    CHECK_EQ(js().eval("markdown('<img src=x onerror=y>').includes('<img')"), "false");
    CHECK_EQ(js().eval("markdown('| <b>x</b> |\\n|---|\\n| y |').includes('<b>')"), "false");
}

TEST(a_fence_still_open_renders_as_code) {
    // A reply mid-stream has an opening fence and no closing one, and it
    // should read as code for the whole time it is arriving.
    CHECK(has("markdown('```c\\nint x;')", "class=\"code\""));
    // The text is there, in pieces: the lexer has colored `int` by now.
    CHECK_EQ(js().eval("markdown('```c\\nint x;').includes('int')"), "true");
}

// --- syntax coloring --------------------------------------------------
    //
    // Carried over from tests/test_syntax.cpp.

TEST(c_and_c_keywords_types_numbers) {
    CHECK(has(highlight("const int x = 42;", "cpp"), "<span class=\"tok-key\">const</span>"));
    CHECK(has(highlight("const int x = 42;", "cpp"), "<span class=\"tok-typ\">int</span>"));
    CHECK(has(highlight("const int x = 42;", "cpp"), "<span class=\"tok-num\">42</span>"));
    CHECK(has(highlight("#include <string>", "c"),   "<span class=\"tok-key\">#include</span>"));
}

TEST(comments_to_the_end_of_the_line_and_block_comments_past_it) {
    CHECK(has(highlight("f(); // note", "cpp"), "<span class=\"tok-com\">// note</span>"));
    CHECK(has(highlight("/* two\nlines */ x", "cpp"),
              "<span class=\"tok-com\">/* two\nlines */</span>"));
    CHECK(has(highlight("x = 1  # note", "py"), "<span class=\"tok-com\"># note</span>"));
    CHECK(has(highlight("-- note", "sql"), "<span class=\"tok-com\">-- note</span>"));
}

TEST(strings_including_the_escaped_quote_that_does_not_end_one) {
    CHECK(has(highlight("s = \"hi\";", "cpp"), "<span class=\"tok-str\">&quot;hi&quot;</span>"));
    CHECK(has(highlight("s = \"a\\\"b\";", "cpp"), "a\\&quot;b"));
    CHECK(has(highlight("`a${b}c`", "ts"), "<span class=\"tok-str\">`a${b}c`</span>"));
}

TEST(python_s_triple_quotes_outlive_their_line) {
    CHECK(has(highlight("\"\"\"doc\nstring\"\"\"", "python"),
              "<span class=\"tok-str\">&quot;&quot;&quot;doc\nstring&quot;&quot;&quot;</span>"));
    CHECK(has(highlight("def f(x):", "py"), "<span class=\"tok-key\">def</span>"));
}

TEST(shell_variables) {
    CHECK(has(highlight("echo $HOME", "bash"), "<span class=\"tok-var\">$HOME</span>"));
    CHECK(has(highlight("echo ${A:-b} x", "sh"), "<span class=\"tok-var\">${A:-b}</span>"));
}

TEST(sql_and_cmake_are_case_folded) {
    CHECK(has(highlight("select a from t", "sql"), "<span class=\"tok-key\">select</span>"));
    CHECK(has(highlight("SELECT a FROM t", "sql"), "<span class=\"tok-key\">SELECT</span>"));
    CHECK(has(highlight("set(X 1)", "cmake"),      "<span class=\"tok-key\">set</span>"));
}

TEST(a_diff_is_colored_by_its_first_column) {
    CHECK(has(highlight("+added\n-gone\n ctx", "diff"), "<span class=\"tok-add\">+added</span>"));
    CHECK(has(highlight("+added\n-gone\n ctx", "diff"), "<span class=\"tok-del\">-gone</span>"));
    CHECK(has(highlight("@@ -1 +1 @@", "patch"),        "<span class=\"tok-com\">@@ -1 +1 @@</span>"));
}

TEST(a_language_it_does_not_know_is_still_escaped) {
    CHECK_EQ(js().eval(highlight("<b>&</b>", "brainfuck")), "&lt;b&gt;&amp;&lt;/b&gt;");
    CHECK_EQ(js().eval(highlight("a < b", "")), "a &lt; b");
}

TEST(the_aliases_a_model_actually_writes) {
    for (const char* lang : {"c++", "cxx", "hpp", "cc"}) {
        CHECK(has(highlight("const x", lang), "tok-key"));
    }
    for (const char* lang : {"py", "python3"}) {
        CHECK(has(highlight("def f", lang), "tok-key"));
    }
    CHECK(has(highlight("fn main", "rs"), "tok-key"));
    CHECK(has(highlight("func main", "golang"), "tok-key"));
}

TEST(html_is_colored_as_tags_attributes_and_the_code_inside_it) {
    const std::string page = "<p class=\"a\" hidden>Tom &amp; Jerry</p><!-- note -->"
                             "<style>.a { color: red; }</style><script>const x = 1;</script>";
    CHECK(has(highlight(page, "html"), "<span class=\"tok-key\">p</span>"));
    CHECK(has(highlight(page, "html"), "<span class=\"tok-typ\">class</span>"));
    CHECK(has(highlight(page, "html"), "<span class=\"tok-str\">&quot;a&quot;</span>"));
    CHECK(has(highlight(page, "html"), "<span class=\"tok-num\">&amp;amp;</span>"));
    CHECK(has(highlight(page, "html"), "<span class=\"tok-com\">&lt;!-- note --&gt;</span>"));
    // What a style and a script hold, as CSS and as JavaScript.
    CHECK(has(highlight(page, "html"), "<span class=\"tok-typ\">color</span>"));
    CHECK(has(highlight(page, "html"), "<span class=\"tok-key\">const</span>"));
    // A value with no quotes is a value, and a less-than is not a tag.
    CHECK(has(highlight("<a href=x>", "htm"), "<span class=\"tok-str\">x</span>"));
    CHECK(!has(highlight("a < b", "html"), "tok-key"));
    CHECK(has(highlight("<?xml version=\"1.0\"?>", "xml"), "<span class=\"tok-typ\">version</span>"));
}

TEST(css_is_colored_by_selector_property_and_value) {
    const std::string css = ".card:hover { margin: 0 -2px 1.5rem; color: #FF8700 !important; }";
    CHECK(has(highlight(css, "css"), "<span class=\"tok-key\">card</span>"));
    CHECK(has(highlight(css, "css"), "<span class=\"tok-typ\">margin</span>"));
    CHECK(has(highlight(css, "css"), "<span class=\"tok-num\">1.5rem</span>"));
    CHECK(has(highlight(css, "css"), "<span class=\"tok-num\">#FF8700</span>"));
    CHECK(has(highlight(css, "scss"), "<span class=\"tok-key\">!important</span>"));
    CHECK(has(highlight("@media (x: 1px) {}", "css"), "<span class=\"tok-key\">@media</span>"));
}

TEST(markdown_colors_its_headings_and_the_code_in_its_fences) {
    const std::string md = "# Title\n\nSee `x` and [docs](https://d.test).\n\n- one\n\n```js\nconst a = 1;\n```";
    CHECK(has(highlight(md, "md"), "<span class=\"tok-key\"># Title</span>"));
    CHECK(has(highlight(md, "md"), "<span class=\"tok-str\">`x`</span>"));
    CHECK(has(highlight(md, "md"), "<span class=\"tok-str\">https://d.test</span>"));
    CHECK(has(highlight(md, "md"), "<span class=\"tok-num\">-</span>"));
    CHECK(has(highlight(md, "markdown"), "<span class=\"tok-key\">const</span>"));
}

TEST(the_languages_a_project_is_built_from_are_all_colored) {
    CHECK(has(highlight("FROM node:20\nRUN echo $HOME", "dockerfile"), "<span class=\"tok-key\">FROM</span>"));
    CHECK(has(highlight("all: $(OBJ)\n\t$(CC) -o $@", "makefile"), "<span class=\"tok-var\">$(CC)</span>"));
    CHECK(has(highlight("set X=1\necho %X%", "bat"), "<span class=\"tok-var\">%X%</span>"));
    CHECK(has(highlight("REM a note", "cmd"), "<span class=\"tok-com\">REM a note</span>"));
    CHECK(has(highlight("function F { param($a) }", "ps1"), "<span class=\"tok-var\">$a</span>"));
    CHECK(has(highlight("<# block #>", "powershell"), "<span class=\"tok-com\">&lt;# block #&gt;</span>"));
    CHECK(has(highlight("public record P(int X);", "cs"), "<span class=\"tok-key\">record</span>"));
    CHECK(has(highlight("guard let x else { return }", "swift"), "<span class=\"tok-key\">guard</span>"));
    CHECK(has(highlight("suspend fun f() {}", "kt"), "<span class=\"tok-key\">suspend</span>"));
    CHECK(has(highlight("defmodule A do end", "ex"), "<span class=\"tok-key\">defmodule</span>"));
    CHECK(has(highlight("{- c -} main = do", "hs"), "<span class=\"tok-com\">{- c -}</span>"));
    CHECK(has(highlight("resource \"a\" \"b\" {}", "tf"), "<span class=\"tok-key\">resource</span>"));
    CHECK(has(highlight("message A { string b = 1; }", "proto"), "<span class=\"tok-typ\">string</span>"));
    CHECK(has(highlight("query { a }", "graphql"), "<span class=\"tok-key\">query</span>"));
    CHECK(has(highlight("type T = keyof U", "ts"), "<span class=\"tok-key\">keyof</span>"));
    CHECK(has(highlight("\\section{A}", "tex"), "<span class=\"tok-key\">\\section</span>"));
    CHECK(has(highlight("node_modules/ # deps", "gitignore"), "<span class=\"tok-com\"># deps</span>"));
}

TEST(a_file_is_read_as_the_language_its_name_says) {
    const auto lang = [](const std::string& path) { return js().eval("languageOf(" + lit(path) + ")"); };
    CHECK_EQ(lang("src/app.py"), std::string("py"));
    CHECK_EQ(lang("Dockerfile"), std::string("dockerfile"));
    CHECK_EQ(lang("build/Makefile"), std::string("makefile"));
    CHECK_EQ(lang("CMakeLists.txt"), std::string("cmake"));
    CHECK_EQ(lang("a\\b\\.gitignore"), std::string("gitignore"));
    CHECK_EQ(lang(".env.local"), std::string("shell"));
    CHECK_EQ(lang("README"), std::string(""));
    // And a block whose fence did not say, that is plainly a page.
    CHECK_EQ(js().eval("guessLanguage(['<!DOCTYPE html>', '<html>'])"), std::string("html"));
}

TEST(coloring_never_loses_or_invents_text) {
    // The one property that matters: strip the spans and the code must be
    // exactly what went in. A lexer that drops a character silently
    // corrupts what the model wrote.
    const std::vector<std::pair<std::string, std::string>> samples = {
        {"cpp",  "auto f = [&](int n) { /* x */ return \"a\" + n; }; // end"},
        {"py",   "def g():\n    \"\"\"d\"\"\"\n    return {1: 'x'}  # t"},
        {"sh",   "for f in *.txt; do echo \"${f}\" $HOME; done # loop"},
        {"rs",   "fn main() { let s: &str = \"hi\"; }"},
        {"sql",  "SELECT a FROM t WHERE b = 'q' -- note"},
        {"json", "{\"k\": [1, 2.5, null]}"},
        {"",     "plain text & nothing"},
        {"cpp",  "\"unterminated"},
        {"cpp",  "/* unterminated"},
        {"py",   "'''unterminated"},
        {"cpp",  ""},
        {"html", "<!DOCTYPE html>\n<p class=a id=\"b\">Tom &amp; <b>x</b></p><!-- c -->\n"
                 "<style>.a { color: #fff; margin: 0 -2px; }</style><script>if (a < b) {}</script>"},
        {"html", "<div class=\"unterminated"},
        {"html", "a < b && c > d <"},
        {"svg",  "<svg viewBox=\"0 0 1 1\"><path d=\"M0 0\"/></svg>"},
        {"css",  ".a:hover, #b > c { font: 12px/1.5 \"Inter\", sans-serif !important; } @media (x: 1px) {}"},
        {"md",   "# T\n\n- `a` and [b](c \"d\")\n\n```py\ndef f(): pass\n```\n> q"},
        {"md",   "```\nunterminated"},
        {"dockerfile", "FROM a:1\nRUN echo $HOME ${X} # c"},
        {"makefile", "all: $(OBJ)\n\t$(CC) $@ $^ # c"},
        {"bat",  "@echo off\nREM c\nset X=%1 %X% %~dp0 %"},
        {"ps1",  "function F { param($a) } <# b #> # c"},
        {"tex",  "\\section{A} 50\\% % c"},
    };
    for (const auto& [lang, text] : samples) {
        const std::string expression =
            "(function(){const h=" + highlight(text, lang) + ";"
            "return h.replace(/<[^>]*>/g,'')"
            ".replace(/&lt;/g,String.fromCharCode(60))"
            ".replace(/&gt;/g,String.fromCharCode(62))"
            ".replace(/&quot;/g,String.fromCharCode(34))"
            ".replace(/&#39;/g,String.fromCharCode(39))"
            // &amp; last, or text that was already an entity unescapes twice.
            ".replace(/&amp;/g,String.fromCharCode(38));})()";
        CHECK_EQ(js().eval(expression), text);
    }
}

// --- a file an expert wants to write ----------------------------------
//
// The one screen where the two answers are not interchangeable, so what it
// shows has to be right: which file, whether it exists, and what would be in
// it afterwards.

TEST(a_new_file_is_shown_whole_and_marked_as_an_addition) {
    const std::string call =
        "pendingEdit({path:'src/hello.py', before:'', after:'print(1)\\nprint(2)'})";
    CHECK(has(call, "New file"));
    CHECK(has(call, "src/hello.py"));
    // The language comes from the extension, since nothing else says.
    CHECK(has(call, "<span class=\"lang\">python</span>"));
    CHECK(has(call, "code-add"));
    CHECK(has(call, "2 lines"));
    CHECK(has(call, "id=\"edit-yes\""));
    CHECK(has(call, "id=\"edit-no\""));
}

TEST(a_change_to_a_file_is_shown_as_the_lines_that_move) {
    const std::string call =
        "pendingEdit({path:'a.py', before:'one\\ntwo', after:'one\\nthree'})";
    CHECK(has(call, "Edit"));
    CHECK_EQ(js().eval(call + ".includes('New file')"), "false");
    // The rest of the file is not what is being decided, so it is not shown
    // whole: one line leaves and one arrives.
    CHECK(has(call, "dl-del"));
    CHECK(has(call, "dl-add"));
    CHECK(has(call, "+1"));
}

TEST(an_edit_that_would_write_nothing_says_so) {
    // An empty code block reporting "1 line" reads as the interface being
    // broken rather than the request being odd.
    CHECK(has("pendingEdit({path:'a.py', before:'', after:''})", "an empty file"));
    CHECK(has("pendingEdit({path:'a.py', before:'x', after:'  '})",
              "this would empty the file"));
    CHECK_EQ(js().eval("pendingEdit({path:'a.py', before:'', after:''}).includes('1 line')"),
             "false");
}

TEST(a_path_from_a_model_is_escaped_like_everything_else) {
    CHECK(has("pendingEdit({path:'<b>x</b>', before:'', after:'y'})", "&lt;b&gt;"));
}

// --- diffs ------------------------------------------------------------
//
// Carried over from tests/test_code_lines.cpp.

TEST(an_unchanged_file_is_all_context) {
    CHECK_EQ(js().eval("diffLines('a\\nb', 'a\\nb').every(l => l.kind === 'same')"), "true");
    CHECK_EQ(js().eval("diffLines('a\\nb', 'a\\nb').length"), "2");
}

TEST(an_added_line_is_marked_and_only_it) {
    CHECK_EQ(js().eval("diffLines('a\\nc', 'a\\nb\\nc')"
                    ".filter(l => l.kind === 'add').map(l => l.text).join()"), "b");
    CHECK_EQ(js().eval("diffLines('a\\nc', 'a\\nb\\nc')"
                    ".filter(l => l.kind === 'del').length"), "0");
}

TEST(a_removed_line_is_marked_and_only_it) {
    CHECK_EQ(js().eval("diffLines('a\\nb\\nc', 'a\\nc')"
                    ".filter(l => l.kind === 'del').map(l => l.text).join()"), "b");
}

TEST(a_changed_line_is_a_removal_and_an_addition) {
    CHECK_EQ(js().eval("diffLines('a\\nold\\nc', 'a\\nnew\\nc')"
                       ".filter(l => l.kind !== 'same')"
                       ".map(l => l.kind + ':' + l.text).join()"),
             "del:old,add:new");
}

TEST(a_new_file_is_all_additions) {
    CHECK_EQ(js().eval("diffLines('', 'a\\nb').filter(l => l.kind === 'add').length"), "2");
}

TEST(the_diff_keeps_every_line_of_both_texts) {
    // Reading the kept and removed lines back gives the first file; the
    // kept and added lines give the second. Anything else means the view
    // is showing a file that does not exist.
    const char* expression =
        "(function(){const d=diffLines('a\\nb\\nc\\nd', 'a\\nx\\nc\\ny\\nz');"
        "const before=d.filter(l=>l.kind!=='add').map(l=>l.text).join('\\n');"
        "const after=d.filter(l=>l.kind!=='del').map(l=>l.text).join('\\n');"
        "return (before==='a\\nb\\nc\\nd') + ',' + (after==='a\\nx\\nc\\ny\\nz');})()";
    CHECK_EQ(js().eval(expression), "true,true");
}

TEST(a_file_too_large_to_diff_says_so_instead_of_hanging) {
    CHECK_EQ(js().eval("diffLines(Array(3000).fill('x').join('\\n'), "
                    "Array(3000).fill('y').join('\\n'))[0].kind"), "note");
}

// --- a build -------------------------------------------------------------
//
// A directive, a plan, an agent per task: the Build view draws the agents
// down one side and the chosen one's work on the other, and the code,
// source and preview panes beside them. Set up last, over the busy state,
// and put back afterwards so that nothing above sees it.

const char* const kBuildState = R"JS(
window.__cookWas = state.snapshot.cook;
state.snapshot.cook = {
  running: true, id: 'b1', kind: 'build', goal: 'a todo CLI in Python', state: 'working', question: '',
  outcome: '', headline: '', iterations: 2, started: 1, ended: 0, seconds: 340, experts: ['physics'],
  files: ['todo.py'], total: 4, shown_from: 0,
  plan: { summary: 'A **Python** CLI.', run: 'python todo.py', check: 'pytest -q', ship: 'make dist' },
  tasks: [
    { index: 0, title: 'Write todo.py', detail: 'adds and lists', needs: 'Python back end', files: ['todo.py'],
      after: [], expert: 'physics', state: 'done', outcome: 'wrote it', started: 1, ended: 2 },
    { index: 1, title: 'Tests', detail: 'pytest', needs: 'testing', files: ['test_todo.py'], after: [0],
      expert: 'math', state: 'working', outcome: '', started: 2, ended: 0 },
    { index: 2, title: 'README', detail: 'how to run it', needs: 'technical writing', files: [], after: [0, 1],
      expert: '', state: 'waiting', outcome: '', started: 0, ended: 0 },
  ],
  steps: [
    { iteration: 1, task: -1, expert: 'claude', kind: 'plan', summary: 'planned 3 tasks', ok: true, ms: 4, detail: '{}' },
    { iteration: 1, task: 0, expert: 'physics', kind: 'write', summary: 'created todo.py', ok: true, ms: 9,
      detail: '@@ line 1 @@\n+print(1)', changed: ['todo.py'] },
    { iteration: 1, task: 0, expert: 'physics', kind: 'screenshot', summary: 'took a screenshot', ok: true, ms: 9,
      picture: '/tmp/shot.png' },
    { iteration: 2, task: 1, expert: 'math', kind: 'run', summary: 'pytest failed', ok: false, ms: 900, detail: 'E' },
  ],
};
state.tree = { root: '~/demo', cut: false, entries: [
  { path: 'src', name: 'src', dir: true, depth: 0, bytes: 0 },
  { path: 'src/todo.py', name: 'todo.py', dir: false, depth: 1, bytes: 120 } ] };
state.open.file = { path: 'src/todo.py', content: 'print(1)\n', language: 'py', bytes: 120, cut: false };
state.source = { git: true, repo: true, branch: 'main', remote: '', ahead: 0, behind: 0, gh: true,
  changes: [{ path: 'src/todo.py', status: 'M', staged: false }] };
state.log = [{ hash: 'abc1234', subject: 'First', author: 'Me', when: '2 hours ago' }];
state.diff = '--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n';
state.open.diff = 'src/todo.py';
state.preview = { pages: ['index.html'], page: 'index.html', url: '', html: '<html><body><h1 id="t">Hi</h1></body></html>',
  inlined: ['site.css'], skipped: [], picking: true, loading: false, error: '',
  picked: { selector: '#t', tag: 'h1', text: 'Hi', styles: { padding: '8px', color: 'rgb(255, 135, 0)' }, box: { w: 100, h: 40 } },
  changes: [{ selector: '#t', prop: 'padding', value: '12px' }] };
state.processes = [{ name: 'p1', command: 'npm run dev', running: true, status: -1, seconds: 42 }];
'ok';
)JS";

TEST(the_build_view_draws_the_agents_and_the_chosen_ones_work) {
    CHECK_EQ(page().eval(kBuildState), "ok");
    CHECK_EQ(drawn("(state.buildPane = 'agents', state.open.task = null, views.build())"), "ok");
    const std::string agents = page().eval("views.build()");
    // One row per task, after the architect's, each saying who has it.
    CHECK(agents.find("1. Write todo.py") != std::string::npos);
    CHECK(agents.find("3. README") != std::string::npos);
    CHECK(agents.find("Architect") != std::string::npos);
    // The working task is the one opened when none was chosen, and its
    // steps are the ones shown: the failed run, not the first task's write.
    CHECK(agents.find("TASK 2") != std::string::npos);
    CHECK(agents.find("pytest failed") != std::string::npos);
    CHECK(agents.find("created todo.py") == std::string::npos);
    // The first task, chosen, shows its file as something to open, and its
    // screenshot as a picture to look at.
    const std::string first = page().eval("(state.open.task = 0, views.build())");
    CHECK(first.find("data-act=\"file-open\" data-path=\"todo.py\"") != std::string::npos);
    CHECK(first.find("data-act=\"picture-open\"") != std::string::npos);
    // The architect's row shows the plan with the commands it named.
    const std::string plan = page().eval("(state.open.task = -1, views.build())");
    CHECK(plan.find("THE PLAN") != std::string::npos);
    CHECK(plan.find("pytest -q") != std::string::npos);
    // The box belongs to the build: Stop and finish, not Send, and its Stop
    // now is the build's own rather than the chat's.
    CHECK(plan.find("Stop and finish") != std::string::npos);
    CHECK(plan.find("data-act=\"build-cancel\"") != std::string::npos);
    // The chat is not shut by a build: its box takes a prompt, and the build
    // is drawn at the foot of the conversation.
    const std::string chat = page().eval("views.chat()");
    CHECK(chat.find("◆ building") != std::string::npos);
    CHECK(chat.find("id=\"prompt\" data-draft") != std::string::npos);
    CHECK(chat.find("placeholder=\"Ask it something\"") != std::string::npos);
}

TEST(the_code_source_and_preview_panes_draw_what_was_read) {
    CHECK_EQ(drawn("(state.buildPane = 'code', views.build())"), "ok");
    const std::string code = page().eval("views.build()");
    CHECK(code.find("todo.py") != std::string::npos);
    CHECK(code.find("data-act=\"editor-edit\"") != std::string::npos);
    // Editing, what is typed is colored as it is typed: the same lexer, drawn
    // under the textarea, which scrolls it.
    page().eval("state.open.file = Object.assign({}, state.open.file,"
                " { path: 'site/index.html', editing: true, draft: '<p class=\"a\">hi</p>' })");
    CHECK_EQ(drawn("views.build()"), "ok");
    const std::string editing = page().eval("views.build()");
    CHECK(editing.find("code-editor-under") != std::string::npos);
    CHECK(editing.find("<span class=\"tok-key\">p</span>") != std::string::npos);
    CHECK(editing.find("data-scroll=\"editor-scroll\"") != std::string::npos);
    page().eval("state.open.file.editing = false; state.open.file.path = 'src/todo.py'");

    page().eval("state.ci = { available: true, runs: ["
                "{ name: 'test', displayTitle: 'Add the parser', status: 'completed', conclusion: 'success', headBranch: 'main', event: 'push' },"
                "{ name: 'test', displayTitle: 'Break it', status: 'completed', conclusion: 'failure', headBranch: 'main', event: 'push' },"
                "{ name: 'lint', displayTitle: 'Now', status: 'in_progress', conclusion: '', headBranch: 'main', event: 'push' }] }");
    CHECK_EQ(drawn("(state.buildPane = 'source', views.build())"), "ok");
    const std::string source = page().eval("views.build()");
    CHECK(source.find("CI ON GITHUB") != std::string::npos);
    CHECK(source.find("✓ passed") != std::string::npos);
    CHECK(source.find("✗ failure") != std::string::npos);
    CHECK(source.find("◐ running") != std::string::npos);
    CHECK(source.find("data-pane=\"preview\"") != std::string::npos);
    page().eval("state.ci = null");
    CHECK(source.find("data-submit=\"git-commit\"") != std::string::npos);
    CHECK(source.find("Publish to GitHub") != std::string::npos);   // gh is there and there is no remote
    CHECK(source.find("abc1234") != std::string::npos);
    CHECK(source.find("make dist") != std::string::npos);           // the plan's ship command, offered

    CHECK_EQ(drawn("(state.buildPane = 'preview', views.build())"), "ok");
    const std::string preview = page().eval("views.build()");
    CHECK(preview.find("<iframe") != std::string::npos);
    CHECK(preview.find("sandbox=\"allow-scripts") != std::string::npos);
    // The picked element and its dials, and the change waiting to be applied.
    CHECK(preview.find("#t") != std::string::npos);
    CHECK(preview.find("id=\"tune-padding\"") != std::string::npos);
    CHECK(preview.find("data-act=\"tune-apply\"") != std::string::npos);
    CHECK(preview.find("padding: 12px") != std::string::npos);
    // What goes to the expert names the page, the element and the change.
    const std::string prompt = page().eval("tunePrompt()");
    CHECK(prompt.find("index.html") != std::string::npos);
    CHECK(prompt.find("`#t`") != std::string::npos);
    CHECK(prompt.find("padding: 12px") != std::string::npos);
}

TEST(a_seat_at_work_in_the_build_opens_its_work) {
    // The build fixture has task 2 being worked by Programming: its seat is
    // lit, says so, and opens the build on that task.
    const std::string side = page().eval("sideView()");
    CHECK(side.find("data-act=\"seat-open\"") != std::string::npos);
    const std::string opened = page().eval(
        "(function () { var t = (state.snapshot.cook.tasks || []).find(function (x) { return x.state === 'working'; });"
        " if (!t) return 'no working task in the fixture';"
        " var was = enter; var went = ''; enter = function (v) { went = v; };"
        " actions['seat-open']({ dataset: { expert: t.expert } }); enter = was;"
        " return went + ':' + state.open.task + ':' + t.index; })()");
    if (opened != "no working task in the fixture") {
        CHECK(opened.rfind("build:", 0) == 0);
        const std::size_t first = opened.find(':', 6);
        CHECK(first != std::string::npos);
        CHECK_EQ(opened.substr(6, first - 6), opened.substr(first + 1));
    }
    // A seat with nothing to do opens its settings.
    const std::string idle = page().eval(
        "(function () { var was = enter; var went = ''; enter = function (v, p) { went = v + '/' + p; };"
        " actions['seat-open']({ dataset: { expert: 'nobody-at-all' } }); enter = was; return went; })()");
    CHECK_EQ(idle, "settings/experts");
}

TEST(the_agents_are_in_the_side_menu_under_the_experts) {
    // With the side menu open the build's agents are in it, under the
    // experts, where they are in sight from every view; the Build pane then
    // leaves its own list out rather than draw them twice.
    page().eval("window.__sideWas = state.sidebar; window.__viewWas = state.view; state.sidebar = 15");
    const std::string side = page().eval("sideView()");
    const std::size_t experts = side.find(">EXPERTS<");
    const std::size_t agents  = side.find(">AGENTS<");
    CHECK(experts != std::string::npos);
    CHECK(agents != std::string::npos);
    CHECK(experts < agents);
    CHECK(side.find("Architect", agents) != std::string::npos);
    CHECK(side.find("data-act=\"agent-open\" data-index=\"1\"", agents) != std::string::npos);
    CHECK(side.find("2. Tests", agents) != std::string::npos);
    const auto times = [](const std::string& text, const std::string& needle) {
        std::size_t n = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
            ++n;
        }
        return n;
    };
    const std::string open = page().eval("(state.view = 'build', state.buildPane = 'agents', views.build())");
    CHECK_EQ(times(open, "1. Write todo.py</span>"), std::size_t{1});
    CHECK(open.find("build-split solo") != std::string::npos);
    CHECK(open.find("TASK 2") != std::string::npos);

    // Folded, the pane draws them itself, and the rail has a dot for the one
    // at work.
    page().eval("state.sidebar = 0");
    const std::string folded = page().eval("views.build()");
    CHECK_EQ(times(folded, "1. Write todo.py</span>"), std::size_t{1});
    CHECK(folded.find("build-split solo") == std::string::npos);
    CHECK(folded.find("data-act=\"agent-open\" data-index=\"1\" data-phase=\"active\"") != std::string::npos);

    // Clicked from another view, an agent opens its work in the Build view.
    const std::string went = page().eval(
        "(function () { state.view = 'chat'; var was = enter; var to = ''; enter = function (v) { to = v; };"
        " actions['agent-open']({ dataset: { index: '0' } }); enter = was; return to + ':' + state.open.task; })()");
    CHECK_EQ(went, "build:0");
    page().eval("state.sidebar = window.__sideWas; state.view = window.__viewWas; state.open.task = null");
}

TEST(the_preview_gives_a_page_its_data_calls_and_makes_it_an_app) {
    // The data calls go in before anything of the page's own, and the picker
    // after all of it.
    const std::string doc = page().eval(
        "tunedDocument('<html><head><title>B</title><script>start()<\\/script></head><body><p>x</p></body></html>')");
    const std::size_t data  = doc.find("window.crucible = {");
    const std::size_t theirs = doc.find("start()");
    const std::size_t picker = doc.find("selectorOf");
    CHECK(data != std::string::npos && theirs != std::string::npos && picker != std::string::npos);
    CHECK(data < theirs);
    CHECK(theirs < picker);

    // A save is kept, and the next load gets it back -- answered to the frame.
    const std::string said = page().eval(
        "(function () { var told = []; var keep = tellFrame; tellFrame = function (m) { told.push(m); };"
        " state.preview.page = 'index.html';"
        " previewMessage({ crucible: 'data', what: 'save', id: 1, data: { rows: [3] } });"
        " previewMessage({ crucible: 'data', what: 'load', id: 2 });"
        " tellFrame = keep; return JSON.stringify(told); })()");
    CHECK(said.find("\"id\":1,\"data\":true") != std::string::npos);
    CHECK(said.find("\"id\":2,\"data\":{\"rows\":[3]}") != std::string::npos);

    // The bar offers to make the page an app; making one asks its name, and
    // what was made offers to open it.
    page().eval("state.buildPane = 'preview'");
    CHECK(page().eval("views.build()").find("data-act=\"app-make\"") != std::string::npos);
    page().eval("state.preview.making = { name: 'Budget', busy: false, can: true }");
    CHECK_EQ(drawn("views.build()"), "ok");
    const std::string making = page().eval("views.build()");
    CHECK(making.find("data-submit=\"app-package\"") != std::string::npos);
    CHECK(making.find("value=\"Budget\"") != std::string::npos);
    page().eval("state.preview.making = null; state.preview.made = { name: 'Budget', where: 'in Applications', launch: '/A/Budget.app' }");
    CHECK_EQ(drawn("views.build()"), "ok");
    CHECK(page().eval("views.build()").find("Open Budget") != std::string::npos);
    page().eval("state.preview.made = null; state.preview.stale = true");
    CHECK(page().eval("views.build()").find("Show it now") != std::string::npos);
    page().eval("state.preview.stale = false");
}

TEST(about_offers_crucibles_own_source_as_a_project) {
    page().eval("state.about = state.about || { version: '0.9.0', update: {}, files: {}, trusted: [] };"
                "state.self = { found: true, root: '/src/crucible', build: '/src/crucible/build',"
                " program: '/src/crucible/build/bin/crucible', why: '', rebuilding: false, status: 0, log: 'built' }");
    CHECK_EQ(drawn("(state.settingsPage = 'about', views.settings())"), "ok");
    const std::string about = page().eval("views.settings()");
    CHECK(about.find("CRUCIBLE'S OWN SOURCE") != std::string::npos);
    CHECK(about.find("data-act=\"self-open\"") != std::string::npos);
    CHECK(about.find("data-act=\"self-restart\"") != std::string::npos);
    page().eval("state.self = { found: false, why: 'this copy was installed from a download' }");
    const std::string none = page().eval("views.settings()");
    CHECK(none.find("installed from a download") != std::string::npos);
    CHECK(none.find("data-act=\"self-choose\"") != std::string::npos);
    page().eval("state.self = null");
}

TEST(history_and_settings_know_about_builds_and_the_computer_switch) {
    CHECK_EQ(drawn("(state.open.cook = state.snapshot.cook, views.history())"), "ok");
    const std::string opened = page().eval("views.history()");
    CHECK(opened.find("DIRECTIVE") != std::string::npos);
    CHECK(opened.find("1. Write todo.py") != std::string::npos);
    page().eval("state.history.cooks.push({ id: 'b0', kind: 'build', tasks: 3, goal: 'a build', state: 'done', when: 'today', files: 2, steps: 9, seconds: 100 })");
    const std::string listed = page().eval("(state.open.cook = null, views.history())");
    CHECK(listed.find("3 tasks") != std::string::npos);
    CHECK(listed.find("<h2>BUILDS</h2>") != std::string::npos);

    page().eval("state.kit = { folder: '/k', pieces: ["
                "{ id: 'gh', label: 'GitHub\\'s command line', why: 'releases', fetched: true, missing: false },"
                "{ id: 'node', label: 'Node.js', why: 'JavaScript', fetched: false, missing: true },"
                "{ id: 'ocr', label: 'Reading text off pictures', why: 'text', fetched: false, missing: false }] }");
    CHECK_EQ(drawn("(state.settingsPage = 'tools', views.settings())"), "ok");
    const std::string tools = page().eval("(state.settingsPage = 'tools', views.settings())");
    CHECK(tools.find("fetched by Crucible") != std::string::npos);
    CHECK(tools.find("this machine has its own") != std::string::npos);
    CHECK(tools.find("data-act=\"kit-fetch\"") != std::string::npos);
    CHECK(tools.find("(Node.js)") != std::string::npos);
    page().eval("state.kit = null");

    // MCP servers: each with its state, and the form to add one.
    page().eval("window.__mcpWas = state.config.tools.mcp;"
                "state.config.tools.mcp = [{ name: 'github', command: 'npx', args: ['-y', 'server-github'],"
                "  env: { GITHUB_TOKEN: '' }, enabled: true }];"
                "state.mcp = { servers: [{ name: 'github', running: true, tools: 26, error: '' }] }");
    CHECK_EQ(drawn("(state.settingsPage = 'tools', views.settings())"), "ok");
    const std::string mcp = page().eval("views.settings()");
    CHECK(mcp.find("MCP SERVERS") != std::string::npos);
    CHECK(mcp.find("npx -y server-github") != std::string::npos);
    CHECK(mcp.find("<span class=\"part\">running</span><span class=\"part\">26 tools</span>") != std::string::npos);
    CHECK(mcp.find("data-submit=\"mcp-add\"") != std::string::npos);
    CHECK_EQ(page().eval(R"JS(JSON.stringify(splitCommand('npx -y "my server" --flag')))JS"),
             "[\"npx\",\"-y\",\"my server\",\"--flag\"]");
    page().eval("state.config.tools.mcp = window.__mcpWas; state.mcp = null");
    CHECK(tools.find("tools-computer_control") != std::string::npos);
    CHECK(tools.find("DuckDuckGo") != std::string::npos);
    CHECK(tools.find("data-act=\"process-stop\"") != std::string::npos);
    const std::string build = page().eval("(state.settingsPage = 'build', views.settings())");
    CHECK(build.find("build.architect") != std::string::npos);
    CHECK(build.find("id=\"worker-model\"") != std::string::npos);
    CHECK(build.find("build-rounds_per_task") != std::string::npos);
    CHECK(build.find("build-agents") != std::string::npos);
    CHECK(build.find("Agents at once") != std::string::npos);

    // Put back, for anything that runs after.
    page().eval("state.snapshot.cook = window.__cookWas; state.open.task = null;");
}
