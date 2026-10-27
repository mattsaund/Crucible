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
#include <map>
#include <string>
#include <vector>

#include "crucible/tools/page_storage.hpp"
#include "test_helpers.hpp"

namespace crucible::gui::web {
extern const unsigned char kRenderJs[];
extern const unsigned int  kRenderJs_size;
extern const unsigned char kPageHtml[];
extern const unsigned int  kPageHtml_size;
#if defined(CRUCIBLE_HAS_KATEX_JS)
extern const unsigned char kKatexJs[];
extern const unsigned int  kKatexJs_size;
#endif
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

/// render.js, as compiled in.
std::string render_source() {
    return std::string(reinterpret_cast<const char*>(crucible::gui::web::kRenderJs),
                       crucible::gui::web::kRenderJs_size);
}

/// One context with scripts already evaluated in it: render.js, and KaTeX
/// ahead of it for the math tests.
///
/// Built once: the tables in that file are the same every time and parsing
/// them per assertion would be the slowest part of this suite.
class Engine {
public:
    explicit Engine(const std::vector<std::string>& sources = {render_source()})
        : ctx_(JSGlobalContextCreate(nullptr)) {
        for (const std::string& source : sources) {
            const Str  script(source);
            JSValueRef thrown = nullptr;
            JSEvaluateScript(ctx_, script, nullptr, nullptr, 0, &thrown);
            if (thrown != nullptr && loaded_error_.empty()) {
                loaded_error_ = to_text(thrown);
            }
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

#if defined(CRUCIBLE_HAS_KATEX_JS)
/// render.js with KaTeX loaded first, the order the page loads them in.
Engine& js_math() {
    static Engine engine({std::string(reinterpret_cast<const char*>(crucible::gui::web::kKatexJs),
                                      crucible::gui::web::kKatexJs_size),
                          render_source()});
    return engine;
}
#endif

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
  compatMode: 'CSS1Compat',
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
state.trainer = { ready: false, present: true, flavor: 'cuda', python: '3.14', torch: '2.14', installed_at: '2026-10-01',
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
    for (const char* view : {"chat", "create", "history", "settings"}) {
        CHECK_EQ(page().eval(std::string("typeof views.") + view), "function");
    }
    // Cook was folded into Build, and Build into Chat: there is no view for
    // either, and a cook's journal is drawn by what draws a build's.
    CHECK_EQ(page().eval("typeof views.cook"), "undefined");
    CHECK_EQ(page().eval("typeof views.build"), "undefined");
}

TEST(every_view_draws_before_anything_has_been_fetched) {
    // The state the page is in for its first frame: no snapshot worth the
    // name, no config, nothing fetched. Every view has to have something to
    // say in it, because every view can be the first one drawn.
    CHECK_EQ(drawn("topView()"), "ok");
    for (const char* view : {"chat", "create", "history", "settings", "models"}) {
        const std::string result = drawn(std::string("views.") + view + "()");
        CHECK_EQ(result, "ok");
    }
    // A pane chosen with no folder to show is the conversation.
    for (const char* pane : {"chat", "code", "source", "preview"}) {
        CHECK_EQ(drawn(std::string("(state.chatPane = '") + pane + "', views.chat())"), "ok");
        CHECK(page().eval("views.chat()").find("id=\"transcript\"") != std::string::npos);
    }
    page().eval("state.chatPane = 'chat'");
    // No project is no longer a wall: the box is open, because a conversation
    // with none open gets a scratch folder of its own -- and opening a project
    // is the right-hand panel's, not the top bar's.
    CHECK(page().eval("topView()").find("open-project") == std::string::npos);
    CHECK(page().eval("views.chat()").find("id=\"prompt\"") != std::string::npos);
}

TEST(the_models_tab_draws_a_search_a_choice_and_downloads) {
    const std::string set = page().eval(R"(
      state.market = { query: 'qwen', format: 'gguf', sort: 'likes', loading: false, more: false,
        error: '', asked: 1, next: 'https://huggingface.co/api/models?cursor=x', option: 'Q4_K_M',
        selected: 'unsloth/Qwen3.8-27B-GGUF', readme: '# Qwen\n\nA <b>model</b>. $E=mc^2$',
        results: [
          { id: 'unsloth/Qwen3.8-27B-GGUF', author: 'unsloth', name: 'Qwen3.8-27B-GGUF', downloads: 5751118,
            likes: 5294, updated: '2026-08-20T12:04:25.000Z', params_b: 27.3, vision: true, tools: true,
            reasoning: true, gated: false, base_model: 'Qwen/Qwen3.8-27B', logo: 'Qwen' },
          { id: 'x/<script>-GGUF', author: 'x', name: '<script>-GGUF', downloads: 0, likes: 0, updated: '',
            params_b: 0, vision: false, tools: false, reasoning: false, gated: true, base_model: '', logo: 'x' } ],
        detail: { id: 'unsloth/Qwen3.8-27B-GGUF', author: 'unsloth', name: 'Qwen3.8-27B-GGUF', format: 'gguf',
          downloads: 5751118, likes: 5294, updated: '2026-08-20T12:04:25.000Z', params_b: 27.3, logo: 'Qwen',
          architecture: 'qwen35', context: 262144, license: 'apache-2.0', base_model: 'Qwen/Qwen3.8-27B',
          vision: true, tools: true, reasoning: true, gated: false,
          machine: { vram: 4e10, ram: 5e10, gpus: 3, apple: false },
          options: [
            { name: 'Q4_K_M', repo: 'unsloth/Qwen3.8-27B-GGUF', bytes: 17e9, files: 2, fit: 'gpu', here: false, recommended: true },
            { name: 'Q8_0', repo: 'unsloth/Qwen3.8-27B-GGUF', bytes: 3e10, files: 2, fit: 'partial', here: true },
            { name: 'BF16', repo: 'unsloth/Qwen3.8-27B-GGUF', bytes: 6e10, files: 3, fit: 'large', here: false } ] } };
      state.snapshot.downloads = [
        { id: 'unsloth/Qwen3.8-27B-GGUF:Q4_K_M', repo: 'unsloth/Qwen3.8-27B-GGUF', option: 'Q4_K_M', format: 'gguf',
          folder: '/m/unsloth/Qwen3.8-27B-GGUF', total: 17e9, done: 4e9, rate: 5e7, state: 'running', error: '' },
        { id: 'a/b:Q2_K', repo: 'a/b', option: 'Q2_K', format: 'gguf', folder: '/m/a/b', total: 1e9, done: 2e8,
          rate: 0, state: 'failed', error: 'not enough space: 1 GB needed, 0 B free' } ];
      state.menu = 'market-option';
      state.logos = { qwen: { image: 'data:image/png;base64,iVBORw0KGgo=', light: false } };
      'set')");
    CHECK_EQ(set, std::string("set"));
    CHECK_EQ(drawn("views.models()"), "ok");
    const std::string html = page().eval("views.models()");
    // Qwen's logo beside its GGUF, in the list and over the README; an
    // initial in the same square where none has come.
    std::size_t logos = 0;
    for (std::size_t at = html.find("<img class=\"logo\" src=\"data:image/png;base64,iVBORw0KGgo=\"");
         at != std::string::npos; at = html.find("<img class=\"logo\"", at + 1)) {
        ++logos;
    }
    CHECK_EQ(logos, std::size_t{2});
    CHECK(html.find("<span class=\"logo\" aria-hidden=\"true\">X</span>") != std::string::npos);
    // The choice, whether it will run, and what is on its way.
    CHECK(html.find("Fits on GPU") != std::string::npos);
    CHECK(html.find("Partial offload") != std::string::npos);
    CHECK(html.find("Too large") != std::string::npos);
    CHECK(html.find("Most likes") != std::string::npos);
    CHECK(html.find("data-act=\"market-cancel\"") != std::string::npos);
    CHECK(html.find("not enough space") != std::string::npos);
    CHECK(html.find("title=\"Suggested\"") != std::string::npos);
    // A name from the hub is text, and a README's HTML is not markup.
    CHECK(html.find("<script>-GGUF") == std::string::npos);
    CHECK(html.find("&lt;script&gt;-GGUF") != std::string::npos);
    CHECK(html.find("<b>model</b>") == std::string::npos);
    page().eval("state.market = null; state.menu = null; state.snapshot.downloads = []; state.logos = null; 'reset'");
}

TEST(logos_are_asked_for_after_the_list_two_at_a_time_and_drawn_when_they_come) {
    const std::string started = page().eval(R"JS(
      (function () {
        window.__keptCall = call; window.__keptRender = render;
        window.__asked = []; window.__answers = {};
        render = function () {};
        call = function (method, params) {
          if (method !== 'market.logo') return Promise.resolve({});
          window.__asked.push(params.id);
          return new Promise(function (ok, fail) { window.__answers[params.id] = { ok: ok, fail: fail }; });
        };
        state.logos = {};
        askLogos([{ id: 'unsloth/a-GGUF', logo: 'Qwen' }, { id: 'bartowski/b-GGUF', logo: 'Qwen' },
                  { id: 'google/c', logo: 'google' }, { id: 'x/d', logo: 'x' }, { id: 'bad', logo: '' }]);
        return 'started';
      })())JS");
    CHECK_EQ(started, "started");
    // Once for each owner, two at a time, in the order the list shows them.
    CHECK_EQ(page().eval("JSON.stringify(window.__asked)"), R"(["unsloth/a-GGUF","google/c"])");
    // A repository opened meanwhile goes ahead of what was waiting.
    page().eval("askLogos([{ id: 'z/f', logo: 'Zeta' }, { id: 'x/d', logo: 'x' }])");
    CHECK_EQ(page().eval("JSON.stringify(logoQueue.map(function (q) { return q.key; }))"), R"(["zeta","x"])");
    // One comes: it is drawn, and the next is asked for.
    page().eval("window.__answers['unsloth/a-GGUF'].ok({ owner: 'Qwen', image: 'data:image/png;base64,iVBORw0KGgo=' })");
    CHECK_EQ(page().eval("JSON.stringify(window.__asked)"), R"(["unsloth/a-GGUF","google/c","z/f"])");
    CHECK(page().eval("repoLogo({ id: 'bartowski/b-GGUF', author: 'bartowski', logo: 'Qwen' })")
              .find("<img class=\"logo\" src=\"data:image/png;base64,iVBORw0KGgo=\"") != std::string::npos);
    // None: the initial stays. A failure: forgotten, to be asked again.
    page().eval("window.__answers['google/c'].ok({ owner: 'google', image: '' })");
    page().eval("window.__answers['z/f'].fail(new Error('Hugging Face could not be reached'))");
    CHECK_EQ(page().eval("repoLogo({ id: 'google/c', author: 'google', logo: 'google' })"),
             "<span class=\"logo\" aria-hidden=\"true\">G</span>");
    CHECK_EQ(page().eval("String('zeta' in state.logos)"), "false");
    CHECK_EQ(page().eval("JSON.stringify(window.__asked)"), R"(["unsloth/a-GGUF","google/c","z/f","x/d"])");
    // A light logo on nothing goes on the dark square rather than on white.
    CHECK(page().eval("(state.logos.recursivesi = { image: 'data:image/webp;base64,UklGRg==', light: true },"
                      " repoLogo({ id: 'unsloth/flint-GGUF', author: 'unsloth', logo: 'RecursiveSI' }))")
              .find("<img class=\"logo on-dark\"") != std::string::npos);
    page().eval("window.__answers['x/d'].ok({ owner: 'x', image: '' })");
    // A repository known only by its id, as a benchmark's model is: asked for
    // by that, and drawn by whose the core says it is.
    page().eval("askLogos([{ id: 'bench/Only-GGUF' }])");
    CHECK_EQ(page().eval("repoLogo({ id: 'bench/Only-GGUF' })"), "<span class=\"logo\" aria-hidden=\"true\">B</span>");
    page().eval("window.__answers['bench/Only-GGUF'].ok({ owner: 'Meta-Llama', image: 'data:image/png;base64,AAAA' })");
    CHECK(page().eval("repoLogo({ id: 'bench/only-gguf' })").find("src=\"data:image/png;base64,AAAA\"") != std::string::npos);
    CHECK_EQ(page().eval("String(state.logos['meta-llama'].image)"), "data:image/png;base64,AAAA");
    CHECK_EQ(page().eval("String('@bench/only-gguf' in state.logos)"), "false");
    CHECK_EQ(page().eval("String(logosAsking)"), "0");
    page().eval("(call = window.__keptCall, render = window.__keptRender, state.logos = null,"
                " delete logoOwners['bench/only-gguf'])");
}

TEST(a_readme_is_tidied_into_what_the_page_draws) {
    const std::string out = page().eval(
        "tidyReadme('\\n---\\nlicense: mit\\n---\\n\\n[![logo](a.png)](https://x)\\n"
        "    Indented by its HTML\\n  - a nested item\\n> [!NOTE]\\n> Said.\\n<img src=x onerror=alert(1)>')");
    // Front matter after a blank line, a link with only a picture in it, and
    // the HTML: gone.
    CHECK(out.find("license") == std::string::npos);
    CHECK(out.find("[](") == std::string::npos);
    CHECK(out.find("<img") == std::string::npos);
    // Indentation the HTML gave is not a code block; a nested item keeps it.
    CHECK(out.find("\nIndented by its HTML") != std::string::npos || out.rfind("Indented by its HTML", 0) == 0);
    CHECK(out.find("  - a nested item") != std::string::npos);
    CHECK(out.find("> **Note**") != std::string::npos);
}

TEST(every_view_draws_a_session_with_everything_in_it) {
    const std::string loaded = page().eval(kBusyState);
    CHECK(loaded.rfind("threw: ", 0) != 0);
    if (loaded.rfind("threw: ", 0) == 0) {
        std::printf("      %s\n", loaded.c_str());
    }
    CHECK_EQ(drawn("topView()"), "ok");
    CHECK_EQ(drawn("sideView()"), "ok");
    for (const char* view : {"chat", "create", "history"}) {
        const std::string result = drawn(std::string("views.") + view + "()");
        CHECK_EQ(result, "ok");
    }
    // A project is open: every pane beside the conversation draws.
    for (const char* pane : {"code", "source", "preview", "chat"}) {
        CHECK_EQ(drawn(std::string("(state.chatPane = '") + pane + "', views.chat())"), "ok");
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
    // And that there is a newer version, on the gear and as a link to it.
    CHECK(top.find("9.9.9 is available") != std::string::npos);
    CHECK(top.find("update-link") != std::string::npos);
    // Four tabs, in order, and no Build among them: a message asking for
    // software becomes a build in the chat.
    const std::string tabs = page().eval(
        "Array.from(topView().matchAll(/data-act=\"view\" data-view=\"([a-z]+)\"/g)).map(m => m[1]).join(',')");
    CHECK_EQ(tabs, std::string("chat,create,models,bench"));
    CHECK_EQ(page().eval("TABS.map(t => t[1]).join(' ')"), std::string("Chat Create Models Bench"));
    // Ctrl+1 to 4 follow them.
    CHECK_EQ(page().eval("(function () { var was = enter, got = [];"
                         " enter = function (view) { got.push(view); };"
                         " ['1', '2', '3', '4', '5'].forEach(function (key) {"
                         "   shortcuts({ ctrlKey: true, key: key, preventDefault: function () {} }); });"
                         " enter = was; return got.join(','); })()"),
             std::string("chat,create,models,bench"));
}

TEST(a_newer_version_is_one_button_away_where_it_can_be_installed) {
    const std::string about = page().eval(
        "(() => { state.about.update.installable = true; state.settingsPage = 'about';"
        " const out = views.settings(); state.about.update.installable = false; return out; })()");
    CHECK(about.find("data-act=\"update-install\"") != std::string::npos);
    CHECK(about.find("Update now") != std::string::npos);
    // Built from source: the command to paste instead.
    const std::string source = page().eval("(state.settingsPage = 'about', views.settings())");
    CHECK(source.find("update-install") == std::string::npos);
    CHECK(source.find("crucible --update") != std::string::npos);
    // On its way: how far, and the button cannot be pressed twice.
    const std::string going = page().eval(
        "(() => { state.about.update.installable = true;"
        " state.updating = { phase: 'downloading', done: 1048576, total: 4194304 };"
        " const out = views.settings(); state.updating = null; state.about.update.installable = false;"
        " return out; })()");
    CHECK(going.find("Downloading 1.0 MB of 4.0 MB") != std::string::npos);
    CHECK(going.find("data-act=\"update-install\" disabled") != std::string::npos);
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
        "  {id: 'python', label: 'Python 3.14.9', state: 'done', detail: '', download: 76270309},"
        "  {id: 'runtime-cuda', label: 'CUDA runtime', state: 'working', detail: 'downloading',"
        "   progress: 0.4, download: 0},"
        "  {id: 'trainer', label: 'Training environment (cuda)', state: 'waiting', detail: '',"
        "   download: 3000000000}]}});"
        " const out = views.chat(); state.snapshot = was; return out; })()");
    CHECK(drawn.find("Setting up") != std::string::npos);
    CHECK(drawn.find("Python 3.14.9") != std::string::npos);
    CHECK(drawn.find("width:40%") != std::string::npos);        // measured, so a bar
    CHECK(drawn.find("Try again") == std::string::npos);        // nothing failed
    CHECK(drawn.find("data-act=\"setup-stop\"") != std::string::npos);   // but it can be stopped
    CHECK(drawn.find("Ask anything") != std::string::npos);     // and the box still works

    // Stopped halfway: it stays up, says so, and carries on when asked.
    const std::string stopped = page().eval(
        "(() => { const was = state.snapshot; state.snapshot = Object.assign({}, was, {turns: [], cook: null,"
        " setup: {running: false, items: ["
        "  {id: 'python', label: 'Python 3.14.9', state: 'stopped', detail: '', download: 76270309},"
        "  {id: 'trainer', label: 'Training environment (cuda)', state: 'waiting', detail: '',"
        "   download: 3000000000}]}});"
        " const out = views.chat(); state.snapshot = was; return out; })()");
    CHECK(stopped.find(">Stopped<") != std::string::npos);
    CHECK(stopped.find("data-act=\"setup-retry\">Resume<") != std::string::npos);
    CHECK(stopped.find("setup-stop") == std::string::npos);
    CHECK(page().eval("typeof actions['setup-stop']") == "function");

    const std::string failed = page().eval(
        "(() => { const was = state.snapshot; state.snapshot = Object.assign({}, was, {turns: [], cook: null,"
        " setup: {running: false, items: ["
        "  {id: 'python', label: 'Python 3.14.9', state: 'failed',"
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
    // And the box has its Auto button on every pane, a build's included.
    for (const char* pane : {"chat", "code", "source", "preview"}) {
        CHECK(page().eval(std::string("(state.chatPane = '") + pane + "', views.chat())")
                  .find("data-act=\"auto-edits\"") != std::string::npos);
    }
    page().eval("state.chatPane = 'chat'");
}

TEST(the_box_draws_what_is_attached_as_tiles_under_a_plus_and_its_menu) {
    // With the build out of the way: asking, it has the box for its answer.
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

    // One box, and one set of tiles: while a build asks, the box is for the
    // answer, and what was attached waits for the next message unshown.
    const std::string answering = page().eval(
        "(function () { state.attached.chat = [{ path: '/p/a.md', name: 'a.md', kind: 'file', label: 'MD' }];"
        " var out = views.chat(); state.attached.chat = []; return out; })()");
    CHECK(answering.find("class=\"tiles\"") == std::string::npos);
    CHECK(answering.find("placeholder=\"answer the question above\"") != std::string::npos);
    CHECK_EQ(page().eval("JSON.stringify(Object.keys(state.attached))"), std::string("[\"chat\"]"));
    CHECK_EQ(page().eval("JSON.stringify(Object.keys(state.drafts))"), std::string("[\"chat\"]"));
}

TEST(a_sent_prompt_and_a_build_show_what_was_attached) {
    const std::string chat = page().eval(
        "(function () { var t = state.snapshot.turns[0]; t.attachments = ["
        " { path: '/p/a.pdf', name: 'a <b>.pdf', label: 'PDF', kind: 'document' }];"
        " turnCache.clear(); var out = views.chat(); delete t.attachments; turnCache.clear(); return out; })()");
    CHECK(chat.find("class=\"attached\"") != std::string::npos);
    CHECK(chat.find("a &lt;b&gt;.pdf") != std::string::npos);
    // A message that became a build shows its attachments over the build.
    const std::string built = page().eval(
        "(function () { var turns = state.snapshot.turns; state.snapshot.turns = [{ prompt: 'make it from this',"
        " reply: '', build: 'c1', streaming: false, failed: false, canceled: false, rev: 9,"
        " attachments: [{ path: '/p/src', name: 'src', label: 'FOLDER', kind: 'folder' }] }];"
        " var out = views.chat(); state.snapshot.turns = turns; return out; })()");
    CHECK(built.find(">FOLDER<") != std::string::npos);
    CHECK(built.find("make it from this") != std::string::npos);
}

TEST(the_drop_overlay_says_where_a_drop_goes_or_why_it_cannot) {
    // The busy state has a build running and asking: the box is for its
    // answer then, and a drop is refused, from whichever view. Working
    // without asking, the box takes a drop whatever the build is doing.
    const char* const on =
        "(function (view, what) { var was = state.view; state.view = view; var out = what();"
        " state.view = was; return out; })";
    CHECK_EQ(page().eval(std::string(on) + "('chat', function () { return dropTarget().why; })"),
             "Answer the build's question first");
    CHECK(page().eval(std::string(on) + "('chat', function () { return dropView(dropTarget()); })")
              .find("Answer the build&#39;s question first") != std::string::npos);
    CHECK_EQ(page().eval(std::string(on)
                         + "('chat', function () { var s = state.snapshot.cook.state;"
                           " state.snapshot.cook.state = 'working'; var mode = dropTarget().mode;"
                           " state.snapshot.cook.state = s; return mode; })"),
             "chat");
    const char* const without_cook =
        "(function (what) { var cook = state.snapshot.cook, view = state.view;"
        " state.snapshot.cook = null; var out = what();"
        " state.snapshot.cook = cook; state.view = view; return out; })";
    CHECK_EQ(page().eval(std::string(without_cook)
                         + "(function () { state.view = 'chat'; return dropTarget().mode; })"),
             "chat");
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

TEST(a_picture_pasted_into_the_box_is_attached_and_text_is_left_to_paste) {
    // What a webview hands the box on Ctrl+V: files, text, or both. A paste
    // that carries files is the box's to attach; one that is text is the
    // box's to paste, which is what it does when nothing stops it.
    const std::string set = page().eval(R"JS(
      (function () {
        window.__calls = [];
        window.__keptCall = call; window.__keptRender = render;
        window.__keptView = state.view; window.__keptCook = state.snapshot.cook;
        render = function () {};
        state.view = 'chat'; state.modal = null; state.snapshot.cook = null; state.error = '';
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
          if (method === 'attach.store') return Promise.resolve({ path: '/kept/' + params.path });
          if (method === 'attach.inspect') {
            return Promise.resolve({ items: params.paths.map(function (p) {
              return { path: p, name: p.split('/').pop(), kind: /\.pdf$/.test(p) ? 'document' : 'image', label: 'X' };
            }) });
          }
          if (method === 'attach.image') return Promise.reject(new Error('no canvas here'));
          return Promise.resolve({});
        };
        window.__file = function (name, type, text) {
          var bytes = Array.from(text).map(function (c) { return c.charCodeAt(0); });
          return { name: name, type: type, size: bytes.length,
                   slice: function (a, b) { return { arrayBuffer: function () {
                     return Promise.resolve(new Uint8Array(bytes.slice(a, b)).buffer); } }; } };
        };
        // Ctrl+V into the box, with `files` and `text` on the clipboard.
        // Says whether the paste was taken from the box. Each file comes
        // as WebKitGTK hands a pasted picture over: as an entry too, whose
        // path is not there.
        window.__paste = function (files, text) {
          var taken = false;
          var box = { disabled: false };
          actions['composer-paste'](box, {
            target: box,
            clipboardData: {
              items: files.map(function (f) { return { kind: 'file', type: f.type,
                getAsFile: function () { return f; },
                webkitGetAsEntry: function () { return { isFile: true, isDirectory: false, name: f.name,
                  file: function (ok, fail) { fail(new Error('Path does not exist')); } }; } }; }),
              files: files,
              getData: function (type) { return type === 'text/plain' ? text || '' : ''; } },
            preventDefault: function () { taken = true; } });
          return taken;
        };
        state.attached.chat = [];
        return 'set';
      })())JS");
    CHECK_EQ(set, "set");

    // A screenshot: attached, by the road a drop takes on Windows and macOS.
    CHECK_EQ(page().eval("String(__paste([__file('image.png', 'image/png', 'png')], ''))"), "true");
    CHECK_EQ(page().eval("JSON.stringify(window.__calls.filter(function (c) { return c[0] === 'attach.store'; })"
                         ".map(function (c) { return [c[1].path, c[1].data]; }))"),
             R"([["image.png","cG5n"]])");
    CHECK_EQ(page().eval("JSON.stringify(state.attached.chat.map(function (t) { return [t.path, t.kind]; }))"),
             R"([["/kept/image.png","image"]])");

    // Text is the box's to paste.
    CHECK_EQ(page().eval("String(__paste([], 'some words'))"), "false");
    // So is a table copied with a picture of itself.
    CHECK_EQ(page().eval("String(__paste([__file('image.png', 'image/png', 'png')], 'Q1\\t120\\nQ2\\t95'))"), "false");
    CHECK_EQ(page().eval("String(state.attached.chat.length)"), "1");

    // Files copied in a file manager, with their names as the text; and a
    // picture with no name, which is named for what it is.
    CHECK_EQ(page().eval("String(__paste([__file('photo.jpg', 'image/jpeg', 'jpg')], 'photo.jpg'))"), "true");
    CHECK_EQ(page().eval("String(__paste([__file('report.pdf', 'application/pdf', '%PDF')], 'report.pdf'))"), "true");
    CHECK_EQ(page().eval("String(__paste([__file('', 'image/png', 'png')], ''))"), "true");
    CHECK_EQ(page().eval("JSON.stringify(state.attached.chat.map(function (t) { return t.path; }))"),
             R"(["/kept/image.png","/kept/photo.jpg","/kept/report.pdf","/kept/pasted.png"])");

    // Where a drop would be refused, so is a paste, and it says why.
    CHECK_EQ(page().eval("(state.modal = { kind: 'confirm' }, String(__paste([__file('a.png', 'image/png', 'a')], '')))"),
             "true");
    CHECK_EQ(page().eval("state.error"), "Close the dialog first");
    CHECK_EQ(page().eval("String(state.attached.chat.length)"), "4");

    page().eval("(call = window.__keptCall, render = window.__keptRender, state.view = window.__keptView,"
                " state.snapshot.cook = window.__keptCook, state.modal = null, state.error = '',"
                " state.attached.chat = [])");
}

TEST(files_copied_in_a_file_manager_paste_as_the_files_where_the_window_says_which) {
    // WebKitGTK pastes what a file manager copied as the paths, as text, or
    // as nothing, and hands the page no file. The window reads the clipboard
    // there and says which files are on it; a paste of those attaches the
    // files themselves, as a drop the window places does, and nothing is
    // copied. Anything else pastes as it did.
    const std::string set = page().eval(R"JS(
      (function () {
        window.__calls = [];
        window.__keptCall = call; window.__keptRender = render;
        window.__keptView = state.view; window.__keptCook = state.snapshot.cook;
        render = function () {};
        state.view = 'chat'; state.modal = null; state.snapshot.cook = null; state.error = '';
        call = function (method, params) {
          window.__calls.push([method, params]);
          if (method === 'attach.inspect') {
            return Promise.resolve({ items: params.paths.map(function (p) {
              return { path: p, name: p.split('/').pop(), kind: 'text', label: 'TXT' }; }) });
          }
          return Promise.resolve({ path: '/kept/copy' });
        };
        // Ctrl+V into the box with `text` on the clipboard and no file handed
        // over. Says whether the box took the paste.
        window.__pasteText = function (text) {
          var taken = false;
          var box = { disabled: false };
          actions['composer-paste'](box, {
            target: box,
            clipboardData: { items: [], files: [],
                             getData: function (type) { return type === 'text/plain' ? text : ''; } },
            preventDefault: function () { taken = true; } });
          return taken;
        };
        state.attached.chat = [];
        return 'set';
      })())JS");
    CHECK_EQ(set, "set");
    const auto paths = [] {
        return page().eval("JSON.stringify(state.attached.chat.map(function (t) { return t.path; }))");
    };

    // Nothing said, as on Windows and macOS: paths pasted are text.
    CHECK_EQ(page().eval("String(__pasteText('/home/me/a b.txt'))"), "false");

    // GNOME Files and Nemo: the paths come as the text. The files are
    // attached where they are.
    page().eval("window.crucibleCopied(['/home/me/a b.txt', '/home/me/pics', 7])");
    CHECK_EQ(page().eval("String(__pasteText('/home/me/a b.txt\\n/home/me/pics'))"), "true");
    CHECK_EQ(paths(), R"(["/home/me/a b.txt","/home/me/pics"])");
    CHECK_EQ(page().eval("JSON.stringify(window.__calls.filter(function (c) { return c[0] === 'attach.inspect'; })"
                         ".map(function (c) { return c[1].paths; }))"),
             R"([["/home/me/a b.txt","/home/me/pics"]])");
    CHECK_EQ(page().eval("String(window.__calls.filter(function (c) { return c[0] === 'attach.store'; }).length)"),
             "0");

    // Dolphin: no text at all, or the URIs.
    page().eval("state.attached.chat = []");
    CHECK_EQ(page().eval("String(__pasteText(''))"), "true");
    CHECK_EQ(paths(), R"(["/home/me/a b.txt","/home/me/pics"])");
    page().eval("state.attached.chat = []");
    CHECK_EQ(page().eval("String(__pasteText('file:///home/me/a%20b.txt\\nfile:///home/me/pics'))"), "true");
    CHECK_EQ(paths(), R"(["/home/me/a b.txt","/home/me/pics"])");

    // Text that names none of them was copied after them.
    page().eval("state.attached.chat = []");
    CHECK_EQ(page().eval("String(__pasteText('a note to self'))"), "false");

    // The middle button pastes the selection, whatever the clipboard holds:
    // WebKitGTK hands the page the clipboard's paths with it all the same.
    CHECK_EQ(page().eval("(middleButton({ button: 1 }), String(__pasteText('/home/me/a b.txt')))"), "false");
    CHECK_EQ(page().eval("String(state.attached.chat.length)"), "0");
    page().eval("middleAt = 0");
    CHECK_EQ(page().eval("(middleButton({ button: 0 }), String(__pasteText('/home/me/a b.txt')))"), "true");
    CHECK_EQ(paths(), R"(["/home/me/a b.txt","/home/me/pics"])");

    // Where a drop would be refused, so is this paste, and it says why.
    page().eval("state.attached.chat = []");
    CHECK_EQ(page().eval("(state.modal = { kind: 'confirm' }, String(__pasteText('/home/me/pics')))"), "true");
    CHECK_EQ(page().eval("state.error"), "Close the dialog first");
    CHECK_EQ(page().eval("String(state.attached.chat.length)"), "0");
    page().eval("state.modal = null; state.error = ''");

    // Something else copied over them: paths are text again.
    page().eval("window.crucibleCopied([])");
    CHECK_EQ(page().eval("String(__pasteText('/home/me/a b.txt'))"), "false");

    page().eval("(call = window.__keptCall, render = window.__keptRender, state.view = window.__keptView,"
                " state.snapshot.cook = window.__keptCook, state.modal = null, state.error = '',"
                " state.attached.chat = [])");
}

TEST(the_empty_chat_suggests_what_fits_where_it_is) {
    const char* const empty =
        "(function (project) { var cook = state.snapshot.cook, was = state.snapshot.project,"
        " runtimes = state.runtimes, turns = state.snapshot.turns;"
        " state.snapshot.cook = null; state.snapshot.project = project; state.snapshot.turns = [];"
        " state.runtimes = { loadable: true, runtimes: [{ installed: true }] };"
        " var out = views.chat();"
        " state.snapshot.cook = cook; state.snapshot.project = was; state.runtimes = runtimes;"
        " state.snapshot.turns = turns; return out; })";
    // A project open: something to ask about it.
    const std::string project =
        page().eval(std::string(empty) + "({ open: true, root: '/home/me/orbit', name: 'orbit' })");
    CHECK(project.find("What is in this project?") != std::string::npos);
    // No project, or a chat's own scratch folder: nothing about files that
    // are not there.
    for (const char* none : {"{ open: false }",
                             "{ open: true, root: '/home/me/.crucible/Scratchpad/1', scratchpad: true }"}) {
        const std::string chat = page().eval(std::string(empty) + "(" + none + ")");
        CHECK(chat.find("Ask anything") != std::string::npos);
        CHECK(chat.find("Explain something to me") != std::string::npos);
        CHECK(chat.find("this project") == std::string::npos);
        CHECK(chat.find("failing test") == std::string::npos);
    }
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
    CHECK(chat.find("placeholder=\"Ask or build anything\"") != std::string::npos);
    CHECK(chat.find("id=\"prompt\" data-draft data-sized data-key=\"composer-key\" data-input=\"composer-grow\"") != std::string::npos);
    CHECK(chat.find("open a project to start") == std::string::npos);
    // Somewhere to start, a build among it: the box builds as well as answers.
    CHECK(chat.find(">Build a small web app<") != std::string::npos);
    // No folder yet, so nothing beside the conversation either.
    CHECK(chat.find("class=\"pane-nav\"") == std::string::npos);
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
    // History is here, under the chats, and no longer in the top bar; open,
    // it keeps the panel beside it, with its button marked.
    CHECK(side.find("data-act=\"view\" data-view=\"history\"") != std::string::npos);
    CHECK(page().eval("topView()").find("data-view=\"history\"") == std::string::npos);
    CHECK(page().eval("(function () { var v = state.view, w = state.recentsWidth; state.view = 'history';"
                      " state.recentsWidth = 16; var out = views.history(); state.view = v; state.recentsWidth = w;"
                      " return out; })()").find("aria-current=\"true\">History</button>") != std::string::npos);
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
    // Each by the model's name, and whose it is on hover.
    CHECK(panel.find(">router<") != std::string::npos);
    CHECK(panel.find(">math<") != std::string::npos);
    CHECK(panel.find("Delegator: router.gguf") != std::string::npos);
    CHECK(panel.find("Mathematics: math.gguf") != std::string::npos);
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

TEST(a_model_is_condensed_from_the_create_tab) {
    // The list offers it beside New expert.
    CHECK(page().eval("(state.open.recipe = null, views.create())").find("data-act=\"condense-new\"")
          != std::string::npos);

    // The dialog: before a model is chosen, while it is measured, and with
    // every size, one suggested and one too big to be worth it.
    const std::string sizes =
        "{ path: '/models/flint-Q6_K.gguf', file: 'flint-Q6_K.gguf', bytes: 32.7e9, bits: 6.58,"
        "  quantization: 'Q6_K', elements: 39.8e9, fit: 'partial', options: ["
        "  { name: 'Q8_0', note: 'near original', bytes: 42.3e9, fit: 'large', worth: false },"
        "  { name: 'Q4_K_M', note: 'balanced', bytes: 24.2e9, fit: 'gpu', worth: true, recommended: true },"
        "  { name: 'Q2_K', note: 'large loss', bytes: 14.6e9, fit: 'gpu', worth: true }] }";
    const std::string models = "state.models = { directory: '/models', display: '/models', models: ["
                               "{ name: 'flint-Q6_K.gguf', path: '/models/flint-Q6_K.gguf', bytes: 32.7e9, format: 'gguf' },"
                               "{ name: 'mmproj-x.gguf', path: '/models/mmproj-x.gguf', bytes: 1e9, format: 'gguf' }] },";
    for (const std::string& modal : {
             std::string("{ kind: 'condense', path: '', sizes: null, pick: '', name: '', purpose: '', error: '' }"),
             std::string("{ kind: 'condense', path: '/models/flint-Q6_K.gguf', measuring: true, sizes: null, pick: '',"
                         " name: '', purpose: '', error: '' }"),
             std::string("{ kind: 'condense', path: '/models/flint-Q6_K.gguf', sizes: ") + sizes
                 + ", pick: 'Q4_K_M', name: 'flint Q4_K_M', purpose: '', error: '' }"}) {
        CHECK_EQ(drawn("(" + models + " modalView(" + modal + "))"), "ok");
    }
    const std::string drawnSizes = page().eval(
        "(" + models + " modalView({ kind: 'condense', path: '/models/flint-Q6_K.gguf', sizes: " + sizes
        + ", pick: 'Q4_K_M', name: 'flint Q4_K_M', purpose: '', error: '' }))");
    CHECK(drawnSizes.find("24.2 GB") != std::string::npos || drawnSizes.find("22.5 GB") != std::string::npos);
    CHECK(drawnSizes.find("Fits on GPU") != std::string::npos);
    CHECK(drawnSizes.find("already this small") != std::string::npos);   // Q8_0 from Q6_K
    CHECK(drawnSizes.find("title=\"Suggested\"") != std::string::npos);
    CHECK(drawnSizes.find("mmproj") == std::string::npos);                // a projector is no model
    // Nothing to condense to until a size is picked and it has a name.
    CHECK_EQ(page().eval("/data-act=\"cz-start\"\\s+disabled/.test((" + models
                         + " modalView({ kind: 'condense', path: '/models/flint-Q6_K.gguf', sizes: " + sizes
                         + ", pick: '', name: '', purpose: '', error: '' })))"), std::string("true"));

    // Named after the file, without its old size.
    CHECK_EQ(page().eval("condenseName('unsloth/flint/flint-Q6_K.gguf', 'Q4_K_M')"), std::string("flint Q4_K_M"));
    CHECK_EQ(page().eval("condenseName('gpt-oss-20b-MXFP4.gguf', 'IQ4_XS')"), std::string("gpt-oss-20b IQ4_XS"));
    CHECK_EQ(page().eval("condenseName('Big-UD-Q8_0-00001-of-00002.gguf', 'Q4_K_M')"), std::string("Big Q4_K_M"));

    // A condensed copy at every stage, opened: its own page, not the
    // fine-tune's method and data.
    for (const char* stage : {"draft", "training", "testing", "finished"}) {
        const std::string recipe = std::string("aRecipe('") + stage + "', { id: 'flint-" + stage + "', method: 'condense',"
            " base: { source: 'local', id: '/models/flint-Q6_K.gguf', path: '/models/flint-Q6_K.gguf', label: 'flint-Q6_K.gguf' },"
            " data: [], quantization: 'Q4_K_M', source_quantization: 'Q6_K', runs: 'gpu', export_bytes: 24e9,"
            " stage_text: '" + (std::string(stage) == "training" ? "condensing" : stage) + "', missing: [] })";
        const std::string page_html = page().eval(
            "(function () { var was = state.recipes; state.recipes = { memory: 6e9, recipes: [" + recipe + "] };"
            " state.open.recipe = 'flint-" + stage + "'; var out = views.create();"
            " state.recipes = was; state.open.recipe = null; return out; })()");
        CHECK(page_html.find("Q6_K") != std::string::npos);
        CHECK(page_html.find("QLoRA") == std::string::npos);
        CHECK(page_html.find("Fits on GPU") != std::string::npos);
        if (std::string(stage) == "training") {
            CHECK(page_html.find("CONDENSING") != std::string::npos);
        }
    }
}

TEST(a_fine_tune_shows_its_score_against_its_base) {
    // The recipe's page, with whatever its last run said about the records
    // it held out of training.
    const auto opened = [](const std::string& held_out) {
        return std::string(
                   "(function () { var was = state.recipes; state.recipes = { memory: 6e9, recipes: ["
                   " aRecipe('testing', { held_out: ") + held_out + " })] };"
               " state.open.recipe = 'kitchen-testing'; var out = views.create();"
               " state.recipes = was; state.open.recipe = null; return out; })()";
    };
    const std::string better = "{ records: 12, base: { loss: 2.314, perplexity: 10.11 },"
                               " tuned: { loss: 1.468, perplexity: 4.34 }, skipped: '', measured: true }";
    CHECK_EQ(drawn(opened(better)), std::string("ok"));
    const std::string scored = page().eval(opened(better));
    CHECK(scored.find("Held out") != std::string::npos);
    CHECK(scored.find("2.31 to 1.47") != std::string::npos);
    CHECK(scored.find("12 records never trained on") != std::string::npos);

    // Worse than the base it came from is said in red.
    const std::string worse = page().eval(opened(
        "{ records: 12, base: { loss: 1.2, perplexity: 3.3 }, tuned: { loss: 1.9, perplexity: 6.7 },"
        " skipped: '', measured: true }"));
    CHECK(worse.find("class=\"bad\"><span title=\"12 records") != std::string::npos);

    // Too little data to hold any out says so; a recipe from before scores
    // says nothing.
    const std::string small = "{ records: 0, base: { loss: 0, perplexity: 0 }, tuned: { loss: 0, perplexity: 0 },"
                              " skipped: 'too little data', measured: false }";
    CHECK_EQ(drawn(opened(small)), std::string("ok"));
    CHECK(page().eval(opened(small)).find("too little data") != std::string::npos);
    CHECK(page().eval(opened("undefined")).find("Held out") == std::string::npos);

    // During the run, the base's number as soon as it is known.
    const std::string running = page().eval(
        "(function () { var run = state.run; state.run = Object.assign({}, run, { held_out: { records: 12,"
        "  base: { loss: 2.314, perplexity: 10.11 }, tuned: { loss: 0, perplexity: 0 }, skipped: '', measured: false } });"
        " state.open.recipe = 'kitchen-training'; var out = views.create();"
        " state.run = run; state.open.recipe = null; return out; })()");
    CHECK(running.find("<span>Held out</span>2.31<") != std::string::npos);
}

TEST(a_model_moved_into_a_subfolder_is_shown_as_itself_not_as_missing) {
    const std::string drawn = page().eval(
        "(function () { var m = state.models; state.models = { directory: '/m', display: '/m',"
        "  models: [{ name: 'Huggingface/GGUF/gpt-oss-20b.gguf', bytes: 1000, format: 'gguf' }] };"
        "  var out = modelSelect('gpt-oss-20b.gguf', '', {}) + '|' + modelSelect('gone.gguf', '', {});"
        "  state.models = m; return out; })()");
    const std::string moved = drawn.substr(0, drawn.find('|'));
    const std::string gone  = drawn.substr(drawn.find('|') + 1);
    CHECK(moved.find("value=\"Huggingface/GGUF/gpt-oss-20b.gguf\" selected") != std::string::npos);
    CHECK(moved.find("not in the models folder") == std::string::npos);
    CHECK(gone.find("not in the models folder") != std::string::npos);
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

TEST(a_code_block_says_what_it_is_and_can_be_copied) {
    // What language at the top, a Copy button beside it, and the code alone:
    // no gutter of numbers, no count of lines.
    const std::string call = "markdown('```python\\na = 1\\nb = 2\\n```')";
    CHECK(has(call, "<span class=\"lang\">python</span>"));
    CHECK(has(call, "data-act=\"copy\""));
    CHECK(has(call, ">Copy</span>"));
    CHECK(!has(call, "code-gutter"));
    CHECK(!has(call, "2 lines"));
    // The code is the fence's lines and nothing else.
    CHECK_EQ(js().eval(call + ".match(/<code>([\\s\\S]*)<\\/code>/)[1].replace(/<[^>]*>/g, '')"),
             "a = 1\nb = 2");
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

TEST(a_fence_written_any_way_people_write_one_is_code_and_never_hangs) {
    // ```c++ once matched neither the fence nor the paragraph, and the loop
    // that reads the lines never moved on: the window stopped mid-reply, and
    // again every time that chat was opened.
    for (const char* src : {"```c++\nint x;\n```", "```c#\nvar x;\n```", "``` python\nx = 1\n```",
                            "````\ncode\n````", "```js title=x\nlet a;\n```", "> ```c++\n> int x;",
                            "~~~\nplain\n~~~", "text\n```c+"}) {
        const std::string html = js().eval("markdown(" + nlohmann::json(src).dump() + ")");
        CHECK(html.find("class=\"code\"") != std::string::npos);
    }
    CHECK_EQ(js().eval(R"(markdown('# Heading\r\ntext'))"), "<h1>Heading</h1><p>text</p>");
    CHECK(js().eval(R"(markdown('```c++\nint x;\n```'))").find("c++") != std::string::npos);
    // A fence closes at a line of its own mark, at least as long: a ```bash
    // inside a ````markdown block is part of the block, not its end.
    const std::string nested = js().eval(R"(markdown('````markdown\n```bash\nls\n```\n````\nafter'))");
    CHECK(nested.find("```bash") != std::string::npos);
    CHECK(nested.find("<p>after</p>") != std::string::npos);
}

TEST(math_without_katex_shows_its_tex) {
    // A build that could not fetch KaTeX, and this file loaded on its own:
    // the formula is still found and shown as what it says, not as markup.
    const std::string html = js().eval(R"(markdown('Energy is $E = mc^2$.'))");
    CHECK(html.find("<code class=\"math-source\">E = mc^2</code>") != std::string::npos);
    CHECK(html.find("katex") == std::string::npos);
}

#if defined(CRUCIBLE_HAS_KATEX_JS)
TEST(katex_loads_ahead_of_render_js) {
    CHECK(js_math().load_error().empty());
    if (!js_math().load_error().empty()) {
        std::printf("      %s\n", js_math().load_error().c_str());
    }
    CHECK_EQ(js_math().eval("typeof katex.renderToString"), "function");
}

TEST(math_is_typeset_in_a_line_and_on_lines_of_its_own) {
    auto has = [](const std::string& html, const char* what) {
        return html.find(what) != std::string::npos;
    };
    const std::string line = js_math().eval(R"(markdown('Energy is $E = mc^2$ and \\(\\pi r^2\\) too.'))");
    CHECK(has(line, "class=\"katex\""));
    CHECK(has(line, "<annotation encoding=\"application/x-tex\">E = mc^2</annotation>"));
    CHECK(has(line, "<annotation encoding=\"application/x-tex\">\\pi r^2</annotation>"));

    const std::string block = js_math().eval(R"(markdown('So:\n\n$$\n\\int_0^1 x\\,dx\n$$\n\nDone.'))");
    CHECK(has(block, "<div class=\"math\">"));
    CHECK(has(block, "katex-display"));
    CHECK(has(block, "<p>Done.</p>"));

    const std::string brackets = js_math().eval(R"(markdown('\\[\n\\sum_{n=1}^\\infty \\frac{1}{n^2}\n\\]'))");
    CHECK(has(brackets, "<div class=\"math\">"));
    const std::string aligned = js_math().eval(R"(markdown('\\begin{align}\na &= b \\\\\nc &= d\n\\end{align}'))");
    CHECK(has(aligned, "<div class=\"math\">"));
    CHECK(!has(aligned, "katex-error"));
}

TEST(a_dollar_that_is_money_stays_money) {
    // Pandoc's rule: "$5, or $10" is two prices, not a formula reading
    // "5, or ". Code keeps its dollars, and \$ is a dollar sign.
    const std::string money = js_math().eval(R"(markdown('It costs $5, or $10 with tax.'))");
    CHECK_EQ(money, "<p>It costs $5, or $10 with tax.</p>");
    CHECK_EQ(js_math().eval(R"(markdown('Set $HOME and $PATH first.'))"),
             "<p>Set $HOME and $PATH first.</p>");
    CHECK_EQ(js_math().eval(R"(markdown('Use `$x$` as is.'))"), "<p>Use <code>$x$</code> as is.</p>");
    CHECK_EQ(js_math().eval(R"(markdown('Price \\$5 and \\$10.'))"), "<p>Price $5 and $10.</p>");
}

TEST(math_is_not_emphasis_and_emphasis_can_hold_math) {
    // In $a_1 + b_2$ the underscores are subscripts. Lifted out before the
    // emphasis runs, so they do not become italics -- and bold around a
    // formula still holds it.
    const std::string html = js_math().eval(R"(markdown('Sum $a_1 + b_2$ and *this* stays.'))");
    std::size_t ems = 0;
    for (std::size_t at = html.find("<em>"); at != std::string::npos; at = html.find("<em>", at + 1)) {
        ++ems;
    }
    CHECK_EQ(ems, std::size_t{1});
    CHECK(html.find("<em>this</em>") != std::string::npos);
    const std::string bold = js_math().eval(R"(markdown('**The value $x$ is 3**'))");
    CHECK(bold.find("<strong>The value <span class=\"katex\">") != std::string::npos);
}

TEST(a_formula_still_arriving_shows_its_tex_until_it_closes) {
    // Drawn half-written, it would flash red with every token.
    const std::string open = js_math().eval(R"(markdown('Here:\n\n$$\n\\frac{a}{'))");
    CHECK(open.find("<pre class=\"math-pending\">$$\n\\frac{a}{</pre>") != std::string::npos);
    CHECK(open.find("katex") == std::string::npos);
    // A stray $$ with a long reply after it is text, not a formula that
    // swallowed the rest of the answer.
    std::string stray = "$$";
    for (int i = 0; i < 80; ++i) {
        stray += "\\nline";
    }
    const std::string text = js_math().eval("markdown('" + stray + "')");
    CHECK(text.find("math-pending") == std::string::npos);
}

TEST(a_formula_that_will_not_parse_is_shown_not_thrown) {
    const std::string html = js_math().eval(R"(markdown('Broken $\\frac{1}{$ here.'))");
    CHECK(html.find("katex-error") != std::string::npos);
    CHECK(html.find("here.") != std::string::npos);
}
#endif

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
    // Both of its lines, whole.
    CHECK_EQ(js().eval(call + ".split('print(').length - 1"), "2");
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
// A message that became a build: a plan, an agent per task, drawn in the
// conversation where the reply would be, the agents down one side and the
// chosen one's work on the other; and the code, source and preview panes
// beside the conversation. Set up last, over the busy state, and put back
// afterwards so that nothing above sees it.

const char* const kBuildState = R"JS(
window.__cookWas = state.snapshot.cook;
if (!window.__turnsWas) window.__turnsWas = state.snapshot.turns;
state.snapshot.turns = window.__turnsWas.concat([{ prompt: 'a todo CLI in Python', reply: '', build: 'b1',
  rev: 41, streaming: false, failed: false, canceled: false, tokens_per_second: 0, prompt_tokens: 0,
  output_tokens: 0, load_ms: 0 }]);
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

TEST(a_message_that_became_a_build_is_drawn_as_the_build_where_its_reply_would_be) {
    CHECK_EQ(page().eval(kBuildState), "ok");
    CHECK_EQ(drawn("(state.chatPane = 'chat', state.open.task = null, state.open.taskOf = '', views.chat())"), "ok");
    // The side menu folded: then the turn lists the agents itself.
    const std::string chat = page().eval(
        "(function () { var w = state.sidebar; state.sidebar = 0; var out = views.chat(); state.sidebar = w;"
        " return out; })()");
    // The message, then the build: how far it is, and one row per task after
    // the architect's, each saying who has it.
    const std::size_t said = chat.find("<div class=\"said\">a todo CLI in Python</div>");
    CHECK(said != std::string::npos);
    CHECK(chat.find("◆ build", said) != std::string::npos);
    CHECK(chat.find("1 of 3 tasks done", said) != std::string::npos);
    CHECK(chat.find("1. Write todo.py", said) != std::string::npos);
    CHECK(chat.find("3. README", said) != std::string::npos);
    CHECK(chat.find("Architect", said) != std::string::npos);
    CHECK(chat.find("id=\"build-b1\"") != std::string::npos);
    // The working task is the one opened when none was chosen, and its
    // steps are the ones shown: the failed run, not the first task's write.
    CHECK(chat.find("TASK 2") != std::string::npos);
    CHECK(chat.find("pytest failed") != std::string::npos);
    CHECK(chat.find("created todo.py") == std::string::npos);
    // The first task, chosen, shows its file as something to open, and its
    // screenshot as a picture to look at.
    const std::string first = page().eval("(state.open.task = 0, state.open.taskOf = 'b1', views.chat())");
    CHECK(first.find("data-act=\"file-open\" data-path=\"todo.py\"") != std::string::npos);
    CHECK(first.find("data-act=\"picture-open\"") != std::string::npos);
    // The architect's row shows the plan with the commands it named.
    const std::string plan = page().eval("(state.open.task = -1, views.chat())");
    CHECK(plan.find("THE PLAN") != std::string::npos);
    CHECK(plan.find("pytest -q") != std::string::npos);
    // A choice is of one build's task: another build's turn goes on showing
    // whichever of its own is at work.
    CHECK(page().eval("(state.open.taskOf = 'another', views.chat())").find("TASK 2") != std::string::npos);
    page().eval("state.open.task = null; state.open.taskOf = ''");
    // Its two stops, in the box whichever pane is up, and Stop now on the
    // turn: the first finishes cleanly, the second is the build's own Stop,
    // not the chat's. The box is not shut by it: it takes a message.
    CHECK(chat.find("data-act=\"cook-stop\"") != std::string::npos);
    CHECK(chat.find("data-act=\"build-cancel\"") != std::string::npos);
    for (const char* pane : {"code", "source", "preview"}) {
        const std::string other = page().eval(std::string("(state.chatPane = '") + pane + "', views.chat())");
        CHECK(other.find("Stop and finish") != std::string::npos);
        CHECK(other.find("data-act=\"build-cancel\"") != std::string::npos);
    }
    page().eval("state.chatPane = 'chat'");
    CHECK(chat.find("id=\"prompt\" data-draft") != std::string::npos);
    CHECK(chat.find("placeholder=\"Ask or build anything\"") != std::string::npos);
    CHECK(chat.find("<textarea id=\"prompt\" data-draft data-sized") != std::string::npos);
    // Asking: the question in its turn, and the box is for the answer.
    const std::string asking = page().eval(
        "(function () { var c = state.snapshot.cook; c.state = 'asking'; c.question = 'Which runner?';"
        " var out = views.chat(); c.state = 'working'; c.question = ''; return out; })()");
    CHECK(asking.find("IT IS ASKING") != std::string::npos);
    CHECK(asking.find("Which runner?") != std::string::npos);
    CHECK(asking.find("placeholder=\"answer the question above\"") != std::string::npos);
    CHECK(asking.find("aria-label=\"Answer\"") != std::string::npos);
}

TEST(a_build_that_ran_before_is_read_for_its_turn) {
    // A conversation opened again: its build is not the one the snapshot
    // carries, so its journal is read from the project's history, and the
    // turn says so until it is. Read, it is drawn whole, its agents in the
    // turn whatever the side menu holds; gone, it is what it left.
    const char* const with =
        "(function (kept) { var turns = state.snapshot.turns, builds = state.builds;"
        " state.snapshot.turns = [{ prompt: 'a calculator', reply: 'It is built.\\n\\nFiles changed: calc.html',"
        "   build: 'old1', rev: 50, streaming: false, failed: false, canceled: false }];"
        " state.builds = kept ? { old1: kept } : {}; var out = views.chat();"
        " state.snapshot.turns = turns; state.builds = builds; return out; })";
    const std::string journal =
        "{ journal: { id: 'old1', kind: 'build', goal: 'a calculator', state: 'done', running: false,"
        " outcome: 'A **calculator**.', files: ['calc.html'], seconds: 90, total: 2, plan: { summary: 'One page.' },"
        " tasks: [{ index: 0, title: 'Write calc.html', detail: 'd', expert: 'math', state: 'done', outcome: 'ok',"
        "   after: [] }], steps: [] } }";
    CHECK(page().eval(std::string(with) + "(null)").find("Reading...") != std::string::npos);
    CHECK_EQ(drawn(std::string(with) + "(" + journal + ")"), "ok");
    const std::string read = page().eval(std::string(with) + "(" + journal + ")");
    CHECK(read.find("1. Write calc.html") != std::string::npos);
    CHECK(read.find("<strong>calculator</strong>") != std::string::npos);
    CHECK(read.find("data-act=\"file-open\" data-path=\"calc.html\"") != std::string::npos);
    CHECK(read.find("id=\"build-old1\"") != std::string::npos);
    const std::string gone = page().eval(std::string(with) + "({ error: 'no cook called old1' })");
    CHECK(gone.find("Files changed: calc.html") != std::string::npos);
}

TEST(a_scratch_folder_shows_its_panes_once_it_has_files) {
    // A chat with no project has its own folder: nothing to show beside the
    // conversation until a turn or a build has put something in it.
    const std::string shown = page().eval(
        "(function () { var p = state.snapshot.project, tree = state.tree;"
        " state.snapshot.project = { open: true, scratchpad: true, root: '/s/1', name: 'Scratchpad' };"
        " state.tree = { entries: [] }; state.chatPane = 'code';"
        " var empty = [folderShown(), shownPane(), paneNav() === ''].join(',');"
        " state.tree = { entries: [{ path: 'index.html', name: 'index.html', dir: false, depth: 0, bytes: 9 }] };"
        " var full = [folderShown(), shownPane(), paneNav() !== ''].join(',');"
        " state.snapshot.project = p; state.tree = tree; state.chatPane = 'chat';"
        " return empty + '|' + full; })()");
    CHECK_EQ(shown, std::string("false,chat,true|true,code,true"));
    // On another pane, the conversation's tab says when it wants somebody:
    // the build asking, or an edit waiting.
    const std::string marked = page().eval(
        "(function () { var c = state.snapshot.cook, e = state.snapshot.pending_edit;"
        " state.snapshot.pending_edit = null; c.state = 'asking'; state.chatPane = 'code';"
        " var asking = paneNav(); c.state = 'working'; var quiet = paneNav();"
        " state.chatPane = 'chat'; var here = (c.state = 'asking', paneNav());"
        " c.state = 'working'; state.snapshot.pending_edit = e; return [asking, quiet, here].join('|'); })()");
    const std::size_t first = marked.find('|');
    const std::size_t second = marked.find('|', first + 1);
    CHECK(marked.substr(0, first).find(">Chat<span class=\"pip\">") != std::string::npos);
    CHECK(marked.substr(first + 1, second - first - 1).find("pip") == std::string::npos);
    CHECK(marked.substr(second + 1).find("pip") == std::string::npos);
}

TEST(the_box_answers_a_build_that_asks_and_is_a_message_otherwise) {
    // What the box sends, by what is going on: an answer to the build's
    // question while it asks, a message (decided by the orchestrator) else.
    const char* const send =
        "(function (cookState, text) { var sent = [], c = state.snapshot.cook, busy = state.snapshot.busy;"
        " var was = call; call = function (m, p) { sent.push(m + ' ' + JSON.stringify(p)); return Promise.resolve({}); };"
        " c.state = cookState; state.snapshot.busy = false;"
        " var box = { value: text, style: {}, offsetHeight: 0, scrollHeight: 0, clientHeight: 0 };"
        " actions.send({ querySelector: function () { return box; }, dataset: { mode: 'chat' } });"
        " call = was; c.state = 'working'; state.snapshot.busy = busy; return sent.join('|'); })";
    CHECK_EQ(page().eval(std::string(send) + "('asking', 'pytest')"),
             std::string("cook.answer {\"answer\":\"pytest\"}"));
    const std::string message = page().eval(std::string(send) + "('working', 'now add dark mode')");
    CHECK(message.rfind("submit {\"prompt\":\"now add dark mode\"", 0) == 0);
    CHECK(message.find("\"answer\"") == std::string::npos);   // decided, not chosen
    // Nothing to answer with, nothing is sent.
    CHECK_EQ(page().eval(std::string(send) + "('asking', '')"), std::string());
    // A model under test in Create is asked questions, never set to work:
    // what is sent there is answered whatever it asks for.
    const std::string tested = page().eval(
        "(function () { var sent = [], was = call, modal = state.modal, busy = state.snapshot.busy;"
        " call = function (m, p) { sent.push(m + ' ' + JSON.stringify(p)); return Promise.resolve({}); };"
        " state.modal = { kind: 'tester', seat: 'lab-kitchen' }; state.snapshot.busy = false;"
        " var input = { value: 'make me a todo app' };"
        " actions['test-send']({ querySelector: function () { return input; } });"
        " call = was; state.modal = modal; state.snapshot.busy = busy; return sent.join('|'); })()");
    CHECK_EQ(tested, std::string("submit {\"prompt\":\"make me a todo app\",\"expert\":\"lab-kitchen\",\"answer\":true}"));
}

TEST(a_thought_opens_from_its_step_as_prose_rather_than_as_output) {
    // A THINK step's line is cut where a word ends, and the whole thought is
    // its detail: opened, it reads as a turn's thinking does.
    const std::string step =
        "({ iteration: 7, expert: 'physics', kind: 'think', ok: true, ms: 120,"
        "   summary: '(only thought) We need to find the header...',"
        "   detail: 'We need to find the header.\\n\\nIt is styled in `style.css`.' })";
    CHECK_EQ(drawn("cookSteps([" + step + "], 0)"), "ok");
    const std::string shut = page().eval("cookSteps([" + step + "], 0)");
    CHECK(shut.find("<details class=\"step\"") != std::string::npos);
    CHECK(shut.find("step-thought") == std::string::npos);   // laid out only when open
    const std::string open = page().eval(
        "(function () { var s = " + step + "; stepsOpen.add(stepKey(s));"
        " var html = cookSteps([s], 0); stepsOpen.delete(stepKey(s)); return html; })()");
    CHECK(open.find("class=\"md step-thought\"") != std::string::npos);
    CHECK(open.find("<code>style.css</code>") != std::string::npos);
    CHECK(open.find("class=\"code") == std::string::npos);
}

TEST(the_code_source_and_preview_panes_draw_what_was_read) {
    // Beside the conversation, switched above it, with a project open.
    const std::string nav = page().eval("(state.chatPane = 'code', views.chat())");
    CHECK(nav.find("class=\"pane-nav\"") != std::string::npos);
    for (const char* pane : {"chat", "code", "source", "preview"}) {
        CHECK(nav.find(std::string("data-act=\"chat-pane\" data-pane=\"") + pane + "\"") != std::string::npos);
    }
    CHECK(nav.find("data-pane=\"code\" aria-current=\"true\"") != std::string::npos);
    CHECK(nav.find("id=\"pane-body\"") != std::string::npos);
    CHECK_EQ(drawn("(state.chatPane = 'code', views.chat())"), "ok");
    const std::string code = page().eval("views.chat()");
    CHECK(code.find("todo.py") != std::string::npos);
    // A file the reader says is not safe to write back is shown, not edited.
    CHECK(code.find("Read only.") != std::string::npos);
    // An editable one opens straight into the editor, colored as it is typed:
    // the same lexer, drawn under the textarea, which scrolls it.
    page().eval("state.open.file = Object.assign({}, state.open.file,"
                " { path: 'site/index.html', editable: true, content: '<p class=\"a\">hi</p>',"
                "   draft: '<p class=\"a\">hi</p>', serial: 7 })");
    CHECK_EQ(drawn("views.chat()"), "ok");
    const std::string editing = page().eval("views.chat()");
    CHECK(editing.find("code-editor-under") != std::string::npos);
    CHECK(editing.find("<span class=\"tok-key\">p</span>") != std::string::npos);
    CHECK(editing.find("data-scroll=\"editor-scroll\"") != std::string::npos);
    CHECK(editing.find("id=\"editor-7\"") != std::string::npos);
    CHECK(editing.find("class=\"ide-gutter\"") != std::string::npos);
    page().eval("state.open.file = { path: 'src/todo.py', content: 'print(1)\\n', language: 'py', bytes: 120, cut: false }");

    page().eval("state.ci = { available: true, runs: ["
                "{ name: 'test', displayTitle: 'Add the parser', status: 'completed', conclusion: 'success', headBranch: 'main', event: 'push' },"
                "{ name: 'test', displayTitle: 'Break it', status: 'completed', conclusion: 'failure', headBranch: 'main', event: 'push' },"
                "{ name: 'lint', displayTitle: 'Now', status: 'in_progress', conclusion: '', headBranch: 'main', event: 'push' }] }");
    CHECK_EQ(drawn("(state.chatPane = 'source', views.chat())"), "ok");
    const std::string source = page().eval("views.chat()");
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

    CHECK_EQ(drawn("(state.chatPane = 'preview', views.chat())"), "ok");
    const std::string preview = page().eval("views.chat()");
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

TEST(the_code_pane_is_an_editor) {
    CHECK_EQ(page().eval(kBuildState), "ok");
    page().eval("state.chatPane = 'code';"
                "state.tree = { root: '~/demo', cut: false, entries: ["
                "  { path: 'src', name: 'src', dir: true, depth: 0, bytes: 0 },"
                "  { path: 'src/todo.py', name: 'todo.py', dir: false, depth: 1, bytes: 120 },"
                "  { path: 'src/build.js', name: 'build.js', dir: false, depth: 1, bytes: 90 },"
                "  { path: 'README.md', name: 'README.md', dir: false, depth: 0, bytes: 40 } ] };"
                "state.code.tabs = [{ path: 'src/todo.py', editable: true, content: 'def a():\\n    return 1\\n',"
                "  draft: 'def a():\\n    return 2\\n', serial: 1, hash: 'h1', indent: '    ', bytes: 24 },"
                "  { path: 'README.md', editable: true, content: '# Hi', draft: '# Hi', serial: 2, hash: 'h2' }];"
                "state.open.file = state.code.tabs[0]; 'ok'");
    const std::string pane = page().eval("views.chat()");
    // Tabs, one marked unsaved; the tree marks it too.
    CHECK(pane.find("class=\"ide-tabs\"") != std::string::npos);
    CHECK(pane.find("dirty-dot") != std::string::npos);
    CHECK(pane.find("data-act=\"tab-close\" data-path=\"README.md\"") != std::string::npos);
    CHECK(pane.find("data-act=\"editor-save\" ") != std::string::npos);
    CHECK(pane.find("Revert") != std::string::npos);
    CHECK(pane.find("Ln 1, Col 1") != std::string::npos);
    CHECK(pane.find(">LF<") != std::string::npos);
    CHECK(pane.find("4 spaces") != std::string::npos);
    CHECK_EQ(page().eval("codeUnsaved().join(',')"), std::string("src/todo.py"));

    // A folder folds, and what is in it goes with it.
    CHECK_EQ(page().eval("(state.code.closed = { src: true }, treeRows(state.tree.entries).length)"), std::string("2"));
    CHECK_EQ(page().eval("(state.code.closed = {}, treeRows(state.tree.entries).length)"), std::string("4"));

    // Find counts what it finds; go to line says how many there are.
    page().eval("state.code.find = { mode: 'replace', query: 'return', replace: 'yield', matchCase: false, at: 0 }");
    const std::string finding = page().eval("views.chat()");
    CHECK(finding.find("1 of 1") != std::string::npos);
    CHECK(finding.find("Replace with") != std::string::npos);
    page().eval("state.code.find = { mode: 'line', query: '', at: 0 }");
    CHECK(page().eval("views.chat()").find("of 3") != std::string::npos);
    page().eval("state.code.find = null");

    // A file an agent changed while it was being edited says so.
    page().eval("state.code.tabs[0].conflict = true");
    CHECK(page().eval("views.chat()").find("Changed on disk") != std::string::npos);
    page().eval("state.code.tabs[0].conflict = false");

    // Search across the project, grouped by file.
    page().eval("state.code.side = 'search'; state.code.search = { query: 'return', matchCase: false, busy: false, cut: false,"
                " results: [{ path: 'src/todo.py', line: 2, text: '    return 1' }, { path: 'src/todo.py', line: 9, text: 'return x' }] }");
    const std::string searching = page().eval("views.chat()");
    CHECK(searching.find("data-act=\"search-open\" data-path=\"src/todo.py\" data-line=\"9\"") != std::string::npos);
    CHECK(searching.find("search-file") != std::string::npos);
    page().eval("state.code.side = 'files'");

    // Quick open finds a file by a few of its letters, its own name first.
    CHECK(std::stoi(page().eval("fuzzy('src/build.js', 'build')")) < 0);   // in the name: ranked ahead
    CHECK_EQ(page().eval("fuzzy('src/todo.py', 'zzz')"), std::string("-1"));
    CHECK_EQ(page().eval("quickMatches('bld')[0].path"), std::string("src/build.js"));
    CHECK_EQ(drawn("modalView({ kind: 'quick-open', query: 'todo', at: 0 })"), "ok");
    CHECK_EQ(drawn("modalView({ kind: 'code-name', title: 'New file', verb: 'Create', value: 'src/', error: 'taken' })"), "ok");

    // The terminal opens under the editor.
    page().eval("state.code.run.open = true; state.code.run.output = '$ ls\\nsrc'");
    CHECK(page().eval("views.chat()").find("id=\"run-command\"") != std::string::npos);
    page().eval("state.code.run.open = false");

    // What it knows about a file's way of writing.
    CHECK_EQ(page().eval("JSON.stringify(indentOf('a:\\n  b\\n    c'))"), std::string("\"  \""));
    CHECK_EQ(page().eval("JSON.stringify(indentOf('func a() {\\n\\tb()\\n}'))"), std::string("\"\\t\""));
    CHECK_EQ(page().eval("commentOf('py') + commentOf('js') + commentOf('sql') + commentOf('md')"), std::string("#//--"));
    CHECK_EQ(page().eval("JSON.stringify(lineSpan('ab\\ncd\\nef', 4, 4))"), std::string("[3,5]"));
    // The banner and the find bar have slots of their own, so neither showing
    // up moves the editor; a file that starts with an empty line keeps it
    // (the parser drops one newline after <textarea>); and a file gone from
    // disk with work in its tab stays open and says so.
    page().eval("state.code.find = null; state.code.tabs[0].conflict = false");
    const std::string slots = page().eval("views.chat()");
    CHECK(slots.find("data-key=\"banner\"") != std::string::npos);
    CHECK(slots.find("data-key=\"find\"") != std::string::npos);
    CHECK_EQ(page().eval("/<textarea[^>]*>\\n/.test(views.chat())"), std::string("true"));
    page().eval("state.code.tabs[0].gone = true");
    CHECK(page().eval("views.chat()").find("Gone from disk") != std::string::npos);
    page().eval("state.code.tabs[0].gone = false");
    // Folded folders are read when first wanted, after the settings are in.
    CHECK_EQ(page().eval("(state.code.closed = null, typeof foldedFolders())"), std::string("object"));
    page().eval("state.code.tabs = []; state.open.file = null; state.chatPane = 'chat'");
}

TEST(closing_the_window_over_unsaved_code_asks_first) {
    // The window is told how many files are unsaved, and only when that
    // changes: with none it closes without asking the page at all.
    page().eval(
        "window.__told = []; window.__closed = [];"
        "window.unsavedWork = function (n) { window.__told.push(n); return Promise.resolve(null); };"
        "window.closeWindow = function (go) { window.__closed.push(go); return Promise.resolve(null); };"
        "state.code.tabs = [{ path: 'src/todo.py', editable: true, content: 'a', draft: 'b', serial: 1 }];"
        "tellUnsaved(); tellUnsaved(); 'ok'");
    CHECK_EQ(page().eval("JSON.stringify(window.__told)"), std::string("[1]"));

    // A close is a question, and one question however often it is asked.
    page().eval("window.crucibleClosing(); window.crucibleClosing(); 'ok'");
    CHECK_EQ(page().eval("state.modal && state.modal.kind"), std::string("confirm"));
    CHECK(page().eval("state.modal.title").find("todo.py") != std::string::npos);
    CHECK_EQ(page().eval("state.modal.body"), std::string("Close anyway?"));
    CHECK_EQ(page().eval("String(state.modalStack.length)"), std::string("0"));
    // Stay keeps the window; Discard closes it.
    page().eval("actions['confirm-no'](); 'ok'");
    CHECK_EQ(page().eval("JSON.stringify(window.__closed)"), std::string("[false]"));
    page().eval("window.crucibleClosing(); 'ok'");
    page().eval("actions['confirm-yes'](); 'ok'");
    CHECK_EQ(page().eval("JSON.stringify(window.__closed)"), std::string("[false,true]"));

    // Saved: the window hears there is nothing to ask about, and a close
    // that reaches the page anyway goes ahead.
    page().eval("state.code.tabs[0].content = 'b'; tellUnsaved(); 'ok'");
    CHECK_EQ(page().eval("JSON.stringify(window.__told)"), std::string("[1,0]"));
    page().eval("window.crucibleClosing(); 'ok'");
    CHECK_EQ(page().eval("String(state.modal)"), std::string("null"));
    CHECK_EQ(page().eval("JSON.stringify(window.__closed)"), std::string("[false,true,true]"));
    page().eval("delete window.unsavedWork; delete window.closeWindow; state.code.tabs = []; 'ok'");
}

TEST(a_seat_at_work_in_the_build_opens_its_work) {
    // The build fixture has task 2 being worked by Mathematics: its seat is
    // lit, says so, and opens the build's turn in the chat on that task,
    // from whichever view it is clicked in.
    const std::string side = page().eval("sideView()");
    CHECK(side.find("data-act=\"seat-open\"") != std::string::npos);
    const std::string opened = page().eval(
        "(function () { var t = (state.snapshot.cook.tasks || []).find(function (x) { return x.state === 'working'; });"
        " var view = state.view; state.view = 'models'; state.chatPane = 'code';"
        " var was = enter; var went = ''; enter = function (v) { went = v; };"
        " actions['seat-open']({ dataset: { expert: t.expert } }); enter = was;"
        " var out = [went, state.open.task === t.index, state.open.taskOf, state.chatPane, state.revealBuild].join(':');"
        " state.view = view; state.revealBuild = ''; return out; })()");
    CHECK_EQ(opened, std::string("chat:true:b1:chat:b1"));
    // A seat with nothing to do opens its settings.
    const std::string idle = page().eval(
        "(function () { var was = enter; var went = ''; enter = function (v, p) { went = v + '/' + p; };"
        " actions['seat-open']({ dataset: { expert: 'nobody-at-all' } }); enter = was; return went; })()");
    CHECK_EQ(idle, "settings/experts");
}

TEST(the_agents_are_in_the_side_menu_under_the_experts) {
    // With the side menu open the build's agents are in it, under the
    // experts, where they are in sight from every view; the build's turn
    // then leaves its own list out rather than draw them twice.
    page().eval("window.__sideWas = state.sidebar; window.__viewWas = state.view; state.sidebar = 15;"
                " state.view = 'chat'; state.chatPane = 'chat'; state.open.task = null; state.open.taskOf = ''");
    const std::string side = page().eval("sideView()");
    const std::size_t experts = side.find(">EXPERTS<");
    const std::size_t agents  = side.find(">AGENTS<");
    CHECK(experts != std::string::npos);
    CHECK(agents != std::string::npos);
    CHECK(experts < agents);
    CHECK(side.find("Architect", agents) != std::string::npos);
    CHECK(side.find("data-act=\"agent-open\" data-index=\"1\" data-build=\"b1\"", agents) != std::string::npos);
    CHECK(side.find("2. Tests", agents) != std::string::npos);
    // At work, it says so in orange: the status line at the top, and under
    // the agent working. Waiting on an answer is not working. The one the
    // conversation shows is marked as open.
    CHECK(side.find("side-status busy") != std::string::npos);
    CHECK(side.find("<span class=\"working\">working</span>") != std::string::npos);
    CHECK(side.find("agent seat here at-work") != std::string::npos);
    const std::string waiting = page().eval(
        "(function () { var was = state.snapshot.cook.state, busy = state.snapshot.busy;"
        " state.snapshot.cook.state = 'asking'; state.snapshot.busy = false;"
        " var out = sideView(); state.snapshot.cook.state = was; state.snapshot.busy = busy; return out; })()");
    CHECK(waiting.find("class=\"working\"") == std::string::npos);
    CHECK(waiting.find("side-status busy") == std::string::npos);
    const auto times = [](const std::string& text, const std::string& needle) {
        std::size_t n = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
            ++n;
        }
        return n;
    };
    const std::string open = page().eval("views.chat()");
    CHECK_EQ(times(open, "1. Write todo.py</span>"), std::size_t{1});
    CHECK(open.find("build-split solo") != std::string::npos);
    CHECK(open.find("TASK 2") != std::string::npos);
    // On another pane, nothing of an agent's shows: none is marked.
    CHECK(page().eval("(state.chatPane = 'code', sideView())").find("agent seat here") == std::string::npos);
    page().eval("state.chatPane = 'chat'");

    // Folded, the turn draws them itself, and the rail has a dot for the one
    // at work.
    page().eval("state.sidebar = 0");
    const std::string folded = page().eval("views.chat()");
    CHECK_EQ(times(folded, "1. Write todo.py</span>"), std::size_t{1});
    CHECK(folded.find("build-split solo") == std::string::npos);
    CHECK(folded.find("data-act=\"agent-open\" data-index=\"1\" data-build=\"b1\" data-phase=\"active\"") != std::string::npos);
    CHECK(folded.find("data-act=\"agent-open\" data-index=\"1\" data-phase=\"active\"") != std::string::npos);

    // Clicked from another view, an agent opens its work in the chat, on the
    // conversation, and its turn is brought into sight.
    const std::string went = page().eval(
        "(function () { state.view = 'models'; state.chatPane = 'source';"
        " var was = enter; var to = ''; enter = function (v) { to = v; };"
        " actions['agent-open']({ dataset: { index: '0', build: 'b1' } }); enter = was;"
        " var out = [to, state.open.task, state.open.taskOf, state.chatPane, state.revealBuild].join(':');"
        " state.revealBuild = ''; return out; })()");
    CHECK_EQ(went, "chat:0:b1:chat:b1");
    page().eval("state.sidebar = window.__sideWas; state.view = window.__viewWas; state.open.task = null;"
                " state.open.taskOf = ''");
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
    page().eval("state.chatPane = 'preview'");
    CHECK(page().eval("views.chat()").find("data-act=\"app-make\"") != std::string::npos);
    page().eval("state.preview.making = { name: 'Budget', busy: false, can: true }");
    CHECK_EQ(drawn("views.chat()"), "ok");
    const std::string making = page().eval("views.chat()");
    CHECK(making.find("data-submit=\"app-package\"") != std::string::npos);
    CHECK(making.find("value=\"Budget\"") != std::string::npos);
    page().eval("state.preview.making = null; state.preview.made = { name: 'Budget', where: 'in Applications', launch: '/A/Budget.app' }");
    CHECK_EQ(drawn("views.chat()"), "ok");
    CHECK(page().eval("views.chat()").find("Open Budget") != std::string::npos);
    page().eval("state.preview.made = null; state.preview.stale = true");
    CHECK(page().eval("views.chat()").find("Show it now") != std::string::npos);
    page().eval("state.preview.stale = false; state.chatPane = 'chat'");
}

namespace {

/// The storage stand-in, as a page is given it: holding `kept`, and putting
/// what it hands over to be kept in `handed`.
std::vector<std::string> stand_in(const std::string& kept) {
    return {"var window = this; var handed = [];",
            "(" + std::string(crucible::tools::kPageStorage) + ")(" + lit(kept)
                + ", function (kind, items) { handed.push(kind + ' ' + JSON.stringify(items)); });"};
}

}  // namespace

TEST(the_storage_stand_in_behaves_as_a_browsers_own) {
    // What a page with no origin of its own is given in place of the
    // localStorage that throws there: in the preview's frame, and in an app.
    // A page written for a browser's has to find it exactly as it would.
    Engine frame(stand_in(R"({"local":{"theme":"dark","count":"3"},"session":{}})"));
    CHECK(frame.load_error().empty());
    CHECK_EQ(frame.eval("typeof localStorage + ' ' + (window.localStorage === localStorage) + ' '"
                        " + ('localStorage' in window) + ' ' + typeof sessionStorage"),
             "object true true object");

    // What it held when the page last ran, in the order it was set.
    CHECK_EQ(frame.eval("localStorage.getItem('theme') + ' ' + localStorage.length"), "dark 2");
    CHECK_EQ(frame.eval("[localStorage.key(0), localStorage.key(1), localStorage.key(2), localStorage.key(-1)].join()"),
             "theme,count,,");
    CHECK_EQ(frame.eval("String(localStorage.getItem('missing') === null)"), "true");

    // Strings in, strings out.
    CHECK_EQ(frame.eval("localStorage.setItem('n', 5); typeof localStorage.getItem('n') + ' ' + localStorage.getItem('n')"),
             "string 5");
    CHECK_EQ(frame.eval("localStorage.setItem('o', { a: 1 }); localStorage.setItem('u', undefined);"
                        " localStorage.getItem('o') + ' ' + localStorage.getItem('u')"),
             "[object Object] undefined");

    // Names as properties, the way the real one has them.
    CHECK_EQ(frame.eval("localStorage.mode = 'list'; localStorage.getItem('mode') + ' ' + localStorage.mode"
                        " + ' ' + ('mode' in localStorage)"),
             "list list true");
    CHECK_EQ(frame.eval("delete localStorage.mode; String(localStorage.getItem('mode')) + ' ' + ('mode' in localStorage)"
                        " + ' ' + String(localStorage.mode)"),
             "null false undefined");
    CHECK_EQ(frame.eval("JSON.stringify(Object.keys(localStorage))"), R"(["theme","count","n","o","u"])");
    CHECK_EQ(frame.eval("JSON.stringify(localStorage)"),
             R"({"theme":"dark","count":"3","n":"5","o":"[object Object]","u":"undefined"})");

    // A name the Storage has already is an item only through getItem.
    CHECK_EQ(frame.eval("localStorage.setItem('getItem', 'x'); localStorage.length = 9;"
                        " typeof localStorage.getItem + ' ' + localStorage.getItem('getItem') + ' '"
                        " + localStorage.getItem('length') + ' ' + localStorage.length + ' '"
                        " + Object.keys(localStorage).indexOf('getItem')"),
             "function x 9 7 -1");
    CHECK_EQ(frame.eval("localStorage.removeItem('getItem'); localStorage.removeItem('length');"
                        " localStorage.removeItem('never'); String(localStorage.length)"),
             "5");

    // Handed over to be kept once a run of changes is over, and only when
    // something changed.
    CHECK_EQ(frame.eval("handed = []; localStorage.setItem('a', '1'); localStorage.setItem('b', '2');"
                        " localStorage.removeItem('a'); String(handed.length)"),
             "0");
    CHECK_EQ(frame.eval("handed.join('|')"),
             R"(local {"theme":"dark","count":"3","n":"5","o":"[object Object]","u":"undefined","b":"2"})");
    CHECK_EQ(frame.eval("handed = []; localStorage.setItem('b', '2'); localStorage.removeItem('a'); 'ok'"), "ok");
    CHECK_EQ(frame.eval("String(handed.length)"), "0");

    // A page testing whether there is storage finds there is.
    const char* const available = R"JS(
      (function (type) {
        var storage;
        try {
          storage = window[type];
          storage.setItem('__storage_test__', '__storage_test__');
          storage.removeItem('__storage_test__');
          return true;
        } catch (e) {
          return e.name === 'QuotaExceededError' && storage && storage.length !== 0;
        }
      }))JS";
    CHECK_EQ(frame.eval(std::string("String(") + available + "('localStorage')) + ' ' + "
                        + available + "('sessionStorage')"),
             "true true");

    // Past the quota a page is told, as a browser tells it, and keeps what
    // it had.
    CHECK_EQ(frame.eval("(function () { try { localStorage.setItem('big', new Array(5000002).join('x')); return 'stored'; }"
                        " catch (e) { return e.name; } })() + ' ' + localStorage.getItem('big')"),
             "QuotaExceededError null");

    // sessionStorage is the other one, handed over as such.
    CHECK_EQ(frame.eval("handed = []; sessionStorage.setItem('step', '2');"
                        " String(localStorage.getItem('step')) + ' ' + sessionStorage.getItem('step')"),
             "null 2");
    CHECK_EQ(frame.eval("handed.join('|')"), R"(session {"step":"2"})");

    // Emptied, and a name that is not an ordinary one.
    CHECK_EQ(frame.eval("handed = []; localStorage.clear(); localStorage.setItem('__proto__', 'p');"
                        " localStorage.getItem('__proto__') + ' ' + localStorage.length"),
             "p 1");
    CHECK_EQ(frame.eval("handed.join('|')"), R"(local {"__proto__":"p"})");

    // Nothing kept that can be read: an empty one, not a broken page.
    Engine fresh(stand_in("not json"));
    CHECK(fresh.load_error().empty());
    CHECK_EQ(fresh.eval("String(localStorage.length) + ' ' + localStorage.getItem('theme')"), "0 null");
}

TEST(the_preview_keeps_what_a_page_puts_in_its_storage_per_project_and_page) {
    // The window hands the page the stand-in, and the frame's document has
    // it after the data calls and before anything of the page's own.
    page().eval("window.cruciblePageStorage = " + lit(std::string(crucible::tools::kPageStorage)));
    const std::string set = page().eval(R"JS(
      (function () {
        window.__keptCall = call; window.__keptRender = render;
        window.__keptProject = state.snapshot.project;
        window.__drawn = 0;
        render = function () { window.__drawn += 1; };
        call = function (method) {
          return Promise.resolve(method !== 'preview.page' ? {} : { inlined: [], skipped: [],
            html: '<html><head><title>T</title><script>start()<\/script></head><body><p>x</p></body></html>' });
        };
        // Open `page` of the project at `root` in the preview.
        window.__show = function (root, page) {
          state.snapshot.project = { open: true, root: root, display: root, name: root.split('/').pop() };
          loadPreview(page);
        };
        // A message from the frame.
        window.__says = function (m) { previewMessage(m); return state.preview.storing; };
        return 'set';
      })())JS");
    CHECK_EQ(set, "set");

    page().eval("__show('/work/budget', 'index.html')");
    CHECK_EQ(page().eval("state.preview.kept"), R"({"local":{},"session":{}})");
    const std::string doc = page().eval("tunedDocument(state.preview.html, state.preview.kept)");
    const std::size_t data    = doc.find("window.crucible = {");
    const std::size_t storage = doc.find("what: 'hello'");
    const std::size_t theirs  = doc.find("start()");
    CHECK(data != std::string::npos && storage != std::string::npos && theirs != std::string::npos);
    CHECK(data < storage);
    CHECK(storage < theirs);

    // The frame run: what the page puts in its storage comes back, after the
    // word that it has started.
    const std::size_t open  = doc.rfind("<script>", storage) + std::strlen("<script>");
    const std::size_t close = doc.find("</script>", storage);
    Engine frame({"var window = this; var posted = [];"
                  " var parent = { postMessage: function (m) { posted.push(JSON.stringify(m)); } };",
                  doc.substr(open, close - open)});
    CHECK(frame.load_error().empty());
    frame.eval("localStorage.setItem('rows', '[3]'); sessionStorage.setItem('tab', 'year'); 'ok'");
    const std::string posted = frame.eval("posted.join('\\n')");
    CHECK(posted.rfind(R"({"crucible":"data","what":"hello"})", 0) == 0);
    page().eval(lit(posted) + ".split('\\n').forEach(function (m) { previewMessage(JSON.parse(m)); }); 'fed'");
    CHECK_EQ(page().eval("previewStorage()"), R"({"local":{"rows":"[3]"},"session":{"tab":"year"}})");

    // Kept per project and page: another project's page of the same name,
    // and another page here, start empty.
    page().eval("__show('/work/other', 'index.html')");
    CHECK_EQ(page().eval("state.preview.kept"), R"({"local":{},"session":{}})");
    page().eval("__show('/work/budget', 'about.html')");
    CHECK_EQ(page().eval("state.preview.kept"), R"({"local":{},"session":{}})");
    page().eval("__show('/work/budget', 'index.html')");
    CHECK_EQ(page().eval("state.preview.kept"), R"({"local":{"rows":"[3]"},"session":{"tab":"year"}})");

    // The document holds what the storage held when it was shown, as a
    // string nothing a page has saved can break out of.
    const std::string shown = page().eval("tunedDocument(state.preview.html, state.preview.kept)");
    CHECK(shown.find(R"(\"rows\":\"[3]\")") != std::string::npos);
    const std::string hostile = page().eval(
        "tunedDocument('<head></head>', JSON.stringify({ local: { k: '</script><script>bad()' }, session: {} }))");
    CHECK(hostile.find("</script><script>bad()") == std::string::npos);
    CHECK(hostile.find("\\u003c/script>\\u003cscript>bad()") != std::string::npos);

    // A page that reloads itself starts again from the document it was shown
    // with. When its storage has changed since, it is shown again holding
    // what it holds now, and what it keeps meanwhile is not taken: it was
    // worked out from the old.
    CHECK_EQ(page().eval("String(__says({ crucible: 'data', what: 'hello' }))"), "true");
    page().eval("__says({ crucible: 'data', what: 'keep', kind: 'local', data: { rows: '[3,4]' } })");
    const std::string drawn_before = page().eval("String(__drawn)");
    CHECK_EQ(page().eval("String(__says({ crucible: 'data', what: 'hello' }))"), "false");
    CHECK(page().eval("String(__drawn)") != drawn_before);
    page().eval("__says({ crucible: 'data', what: 'keep', kind: 'local', data: { rows: '[]' } })");
    CHECK_EQ(page().eval("previewStorage()"), R"({"local":{"rows":"[3,4]"},"session":{"tab":"year"}})");
    CHECK_EQ(page().eval("state.preview.kept"), R"({"local":{"rows":"[3,4]"},"session":{"tab":"year"}})");
    CHECK_EQ(page().eval("String(__says({ crucible: 'data', what: 'hello' }))"), "true");
    page().eval("__says({ crucible: 'data', what: 'keep', kind: 'local', data: { rows: '[3,4,5]' } })");
    CHECK_EQ(page().eval("previewStorage()"), R"({"local":{"rows":"[3,4,5]"},"session":{"tab":"year"}})");

    // window.crucible.save, beside it and apart from it.
    page().eval("__says({ crucible: 'data', what: 'save', id: 1, data: { total: 9 } })");
    CHECK_EQ(page().eval("JSON.stringify(previewKept('data', null)) + ' ' + previewStorage()"),
             R"({"total":9} {"local":{"rows":"[3,4,5]"},"session":{"tab":"year"}})");

    page().eval("(call = window.__keptCall, render = window.__keptRender, state.snapshot.project = window.__keptProject,"
                " Object.assign(state.preview, { page: 'index.html', shown: null, kept: '{}', storing: false }),"
                " delete window.cruciblePageStorage)");
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
    // One left asking when Crucible closed was interrupted; the one running
    // now is still working.
    page().eval("state.history.cooks.push({ id: 'b9', kind: 'build', tasks: 8, goal: 'left', state: 'asking',"
                " when: 'yesterday', files: 0, steps: 2, seconds: 0 })");
    const std::string left = page().eval("views.history()");
    CHECK(left.find("<span class=\"part\">interrupted</span>") != std::string::npos);
    CHECK_EQ(page().eval("settled({ id: state.snapshot.cook.id, state: 'working' })"), std::string("working"));

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
    page().eval("state.snapshot.cook = window.__cookWas; state.open.task = null;"
                " state.snapshot.turns = window.__turnsWas || state.snapshot.turns;");
}

TEST(the_server_page_says_where_it_is_and_what_it_has_answered) {
    // Off, and before the engine has said anything about it.
    page().eval("window.__serverWas = state.snapshot.server; delete state.snapshot.server;"
                "state.config.server = { enabled: false, model: '', port: 8338, network: false, api_key: '' }");
    CHECK_EQ(drawn("(state.settingsPage = 'server', views.settings())"), "ok");
    const std::string off = page().eval("views.settings()");
    CHECK(off.find("<h1>Server</h1>") != std::string::npos);
    CHECK(off.find("data-page=\"server\"") != std::string::npos);
    CHECK(off.find("Delegator (router.gguf)") != std::string::npos);
    CHECK(off.find("f-server-port") != std::string::npos);
    CHECK(off.find("f-server-network") != std::string::npos);
    CHECK(off.find("ADDRESSES") == std::string::npos);
    CHECK(off.find("data-act=\"server-copy\"") == std::string::npos);

    // On the network with a key, answering one request with two behind it.
    page().eval("state.config.server = { enabled: true, model: 'physics.gguf', port: 8338, network: true,"
                "  api_key: 'sk-local' };"
                "state.snapshot.server = { enabled: true, listening: true, port: 8338, network: true, key: true,"
                "  error: '', model: 'physics.gguf', answering: true, waiting: 'the chat', queued: 2,"
                "  addresses: { local: 'http://localhost:8338/v1', network: 'http://192.168.1.20:8338/v1' },"
                "  lan: '192.168.1.20',"
                "  recent: [{ at: 1761500000, model: 'physics.gguf', path: '/v1/chat/completions',"
                "             from: '192.168.1.30', status: 200, prompt: 12, completion: 40, ms: 1500, stream: true },"
                "           { at: 1761500100, model: '', path: '/v1/chat/completions', from: '192.168.1.31',"
                "             status: 401, prompt: 0, completion: 0, ms: 0, stream: false }] }");
    CHECK_EQ(drawn("views.settings()"), "ok");
    const std::string on = page().eval("views.settings()");
    CHECK(on.find("Waiting for the chat, 2 queued") != std::string::npos);
    CHECK(on.find("<code>http://localhost:8338/v1</code>") != std::string::npos);
    CHECK(on.find("<code>http://192.168.1.20:8338/v1</code>") != std::string::npos);
    // What another computer is given, plainly: the IP and the port.
    CHECK(on.find("IP and port <code>192.168.1.20:8338</code>") != std::string::npos);
    // A copy for each address and the IP and port, and one for the key,
    // which is never on the page as text.
    CHECK_EQ(page().eval("(views.settings().match(/data-act=\"server-copy\"/g) || []).length"), "4");
    // With the network off, the IP and port is still there to read, marked
    // off and with nothing to copy.
    CHECK(page().eval("(function () { var s = state.snapshot.server, n = s.network, a = s.addresses;"
                      " s.network = false; s.addresses = { local: a.local }; var out = views.settings();"
                      " s.network = n; s.addresses = a; return out; })()")
              .find("IP and port <code>192.168.1.20:8338</code> <span class=\"tag\">off</span>") != std::string::npos);
    CHECK(on.find("type=\"password\" data-path=\"server.api_key\"") != std::string::npos);
    // The expert's model, chosen under the expert's name.
    CHECK(on.find("value=\"physics.gguf\" selected>Physics: physics.gguf") != std::string::npos);
    CHECK(on.find("<span class=\"part\">12 in</span><span class=\"part\">40 out</span>"
                  "<span class=\"part\">1.5 s</span>") != std::string::npos);
    CHECK(on.find("<span class=\"bad\">401</span>") != std::string::npos);
    CHECK(on.find("title=\"192.168.1.30\"") != std::string::npos);

    // Loading, and then a port somebody else has.
    page().eval("state.snapshot.server.waiting = 'loading'; state.snapshot.server.queued = 0");
    CHECK(page().eval("views.settings()").find("Loading</div>") != std::string::npos);
    page().eval("state.snapshot.server.listening = false; state.snapshot.server.answering = false;"
                "state.snapshot.server.error = 'Port 8338 is in use'; state.snapshot.server.addresses = {}");
    CHECK_EQ(drawn("views.settings()"), "ok");
    const std::string taken = page().eval("views.settings()");
    CHECK(taken.find("<span class=\"bad\">Port 8338 is in use</span>") != std::string::npos);
    CHECK(taken.find("ADDRESSES") == std::string::npos);

    // Put back, for anything that runs after.
    page().eval("state.snapshot.server = window.__serverWas; delete state.config.server;"
                "state.settingsPage = 'general'");
}

/// Results as the runner keeps them: two benchmarks, two skills.
const char* const kBenchState = R"JS(
window.__benchResult = function (id, a, b) {
  return { id: id, schema: 1, suite: 'crucible-bench-1', size: 'quick', date: '2026-10-26',
           model: { file: id + '-Q4_K_M.gguf', name: 'Tiny ' + id, quant: 'Q4_K_M', sha256: 'ab' },
           benchmarks: { mmlu_pro: { n: 50, correct: a }, gsm8k: { n: 50, correct: b } },
           skills: { knowledge: a / 50, math: b / 50 }, overall: (a / 50 + b / 50) / 2,
           speed: { gen_tps: 38.1, prompt_tps: 512.3 }, seconds: 600, hardware: { backend: 'Vulkan' } };
};
window.__modelsWas = state.models;
state.models = { directory: '/m', display: '/m', models: [
  { name: 'tiny-Q4_K_M.gguf', path: '/m/tiny-Q4_K_M.gguf', bytes: 7e8, format: 'gguf' },
  { name: 'mmproj-tiny.gguf', path: '/m/mmproj-tiny.gguf', bytes: 1e8, format: 'gguf' },
  { name: 'tiny-mlx', path: '/m/tiny-mlx', bytes: 1e9, format: 'mlx' }] };
state.bench.results = { results: [__benchResult('r1', 31, 44), __benchResult('r2', 20, 30),
                                  __benchResult('r3', 40, 45), __benchResult('r4', 10, 10)] };
state.bench.board = { offline: false, entries: [
  { id: 12, size: 'quick', model: { file: '<img src=x onerror=alert(1)>.gguf', repo: 'someone/big-GGUF', sha256: 'cd' },
    benchmarks: { arc: { n: 50, correct: 40 } }, skills: { reasoning: 0.8 }, overall: 0.8,
    posted: { user: 'someone', issue: 12 } }] };
'ok'
)JS";

TEST(the_bench_tab_runs_compares_and_posts) {
    CHECK_EQ(page().eval(kBenchState), "ok");
    // Every state of the tab draws: before anything is read, with results and
    // the board, mid run, and with a result opened.
    CHECK_EQ(drawn("(function () { var r = state.bench.results, b = state.bench.board;"
                   " state.bench.results = null; state.bench.board = null; var out = views.bench();"
                   " state.bench.results = r; state.bench.board = b; return out; })()"), "ok");
    CHECK_EQ(drawn("views.bench()"), "ok");

    // A run is set up on the screen behind Add benchmark, and only what can
    // be run is offered there: no projector, no MLX.
    CHECK(page().eval("views.bench()").find("data-act=\"bench-start\"") == std::string::npos);
    page().eval("actions['bench-add']()");
    CHECK_EQ(drawn("views.bench()"), "ok");
    const std::string tab = page().eval("views.bench()");
    CHECK(tab.find("data-act=\"bench-home\"") != std::string::npos);   // the way back
    CHECK(tab.find("YOURS") != std::string::npos);
    CHECK(tab.find("data-path=\"/m/tiny-Q4_K_M.gguf\"") != std::string::npos);
    CHECK(tab.find("mmproj") == std::string::npos);
    CHECK(tab.find("tiny-mlx") == std::string::npos);
    CHECK_EQ(page().eval("/data-act=\"bench-start\"\\s+disabled/.test(views.bench())"), std::string("true"));
    CHECK(page().eval("views.bench()").find("data-act=\"bench-effort\"") != std::string::npos);
    page().eval("state.bench.picked['/m/tiny-Q4_K_M.gguf'] = true");
    CHECK(page().eval("views.bench()").find("450 questions") != std::string::npos);
    page().eval("state.bench.size = 'standard'");
    CHECK(page().eval("views.bench()").find(std::to_string(8 * 200 + 164) + " questions") != std::string::npos);
    page().eval("state.bench.size = 'quick'; state.bench.off.humaneval = true; state.bench.off.mbpp = true");
    CHECK(page().eval("views.bench()").find("Code tests run here") == std::string::npos);
    page().eval("state.bench.off = {}; actions['bench-home']()");

    // The board's arithmetic: the README's own example.
    CHECK_EQ(page().eval("JSON.stringify(benchSkills({ mmlu_pro: { n: 50, correct: 31 }, gsm8k: { n: 50, correct: 44 } }))"),
             std::string("{\"knowledge\":0.62,\"math\":0.88}"));
    CHECK_EQ(page().eval("benchOverall({ knowledge: 0.62, math: 0.88 }).toFixed(2)"), std::string("0.75"));

    // Three on the chart at most. A model keeps its color, and the one that
    // makes room gives its color to the newcomer.
    page().eval("var was = render; render = function () {};"
                "['r1', 'r2', 'r3'].forEach(function (id) { actions['bench-compare']({ dataset: { id: id } }); });"
                "render = was");
    CHECK_EQ(page().eval("state.bench.compare.map(function (c) { return c.key + c.color; }).join(' ')"),
             std::string("r1#3987e5 r2#d55181 r3#008300"));
    page().eval("var was = render; render = function () {}; actions['bench-compare']({ dataset: { id: 'r4' } }); render = was");
    CHECK_EQ(page().eval("state.bench.compare.map(function (c) { return c.key + c.color; }).join(' ')"),
             std::string("r2#d55181 r3#008300 r4#3987e5"));
    const std::string chart = page().eval("views.bench()");
    CHECK_EQ(drawn("views.bench()"), "ok");
    CHECK(chart.find("<svg class=\"radar\"") != std::string::npos);
    CHECK(chart.find("stroke=\"#d55181\"") != std::string::npos);
    CHECK(chart.find("stroke=\"#3987e5\"") != std::string::npos);
    CHECK(chart.find("aria-label=\"r3-Q4_K_M, Math: 90.0%\"") != std::string::npos);
    CHECK(chart.find("class=\"bench-table\"") != std::string::npos);
    // Text wears text colors: the series color is only ever a mark's.
    CHECK(chart.find("color:#") == std::string::npos);

    // What other people posted is text, never markup.
    page().eval("var was = render; render = function () {}; actions['bench-compare-board']({ dataset: { id: '12' } }); render = was");
    const std::string board = page().eval("views.bench()");
    CHECK(board.find("<img src=x") == std::string::npos);
    CHECK(board.find("&lt;img src=x onerror=alert(1)&gt;") != std::string::npos);
    CHECK(board.find("someone") != std::string::npos);
    CHECK(board.find("partial") != std::string::npos);   // one skill of seven
    // A row that is not a result is skipped rather than taking the page down.
    CHECK_EQ(drawn("(function () { var b = state.bench.board; state.bench.board = { entries: [null, 5, 'x', [],"
                   " b.entries[0]] }; var out = benchBoardView(); state.bench.board = b; return out; })()"), "ok");
    page().eval("state.bench.boardQuery = 'nothing like it'");
    CHECK(page().eval("views.bench()").find("Nothing matches.") != std::string::npos);
    // The board names a model by its file and its repository on Hugging Face.
    page().eval("state.bench.boardQuery = 'someone/big'");
    CHECK(page().eval("views.bench()").find("Nothing matches.") == std::string::npos);
    CHECK(page().eval("views.bench()").find("title=\"someone/big-GGUF, &lt;img") != std::string::npos);
    page().eval("state.bench.boardQuery = ''; state.bench.compare = []");

    // A run on its way: a bar per benchmark, and how long is left, on the add
    // screen; the board says only that one is running.
    page().eval("state.snapshot.bench = { running: true, id: 'x', size: 'quick', started: Date.now() / 1000 - 120,"
                " benchmarks: ['mmlu_pro'], models: [] }");
    CHECK(page().eval("views.bench()").find("<span class=\"working\">Running</span>") != std::string::npos);
    page().eval("actions['bench-add']()");
    page().eval("state.snapshot.bench = { running: true, id: 'x', size: 'quick', started: Date.now() / 1000 - 120,"
                " benchmarks: ['mmlu_pro', 'gsm8k'], models: [{ path: '/m/tiny-Q4_K_M.gguf', name: 'tiny-Q4_K_M.gguf',"
                " state: 'running', started: Date.now() / 1000 - 120, current: 'gsm8k', benchmarks: {"
                " mmlu_pro: { done: 50, n: 50, correct: 30 }, gsm8k: { done: 10, n: 50, correct: 8 } } }] }");
    CHECK_EQ(drawn("views.bench()"), "ok");
    const std::string live = page().eval("views.bench()");
    CHECK(live.find("60 of 100 questions") != std::string::npos);
    CHECK(live.find("about 1 min left") != std::string::npos);   // 40 to go at 2 seconds each
    // A quick model done in a minute, then one taking 100 seconds a question:
    // 90 to go at its pace, not at the whole run's.
    page().eval("state.snapshot.bench = { running: true, id: 'x', size: 'quick', started: Date.now() / 1000 - 1060,"
                " benchmarks: ['mmlu_pro', 'gsm8k'], models: [{ path: '/m/tiny.gguf', name: 'tiny.gguf', state: 'done',"
                " started: Date.now() / 1000 - 1060, benchmarks: { mmlu_pro: { done: 50, n: 50, correct: 30 },"
                " gsm8k: { done: 50, n: 50, correct: 40 } } }, { path: '/m/big.gguf', name: 'big.gguf', state: 'running',"
                " started: Date.now() / 1000 - 1000, current: 'mmlu_pro', benchmarks: { mmlu_pro: { done: 10, n: 50, correct: 8 } } }] }");
    CHECK(page().eval("views.bench()").find("about 2h 30m left") != std::string::npos);
    CHECK(live.find("data-act=\"bench-stop\"") != std::string::npos);
    page().eval("state.snapshot.bench.fetching = 'truthfulqa'");
    CHECK(page().eval("views.bench()").find("Fetching TruthfulQA") != std::string::npos);
    page().eval("delete state.snapshot.bench.fetching");
    page().eval("state.snapshot.bench.running = false; state.snapshot.bench.stopped = true;"
                "state.snapshot.bench.ended = state.snapshot.bench.started + 60");
    CHECK(page().eval("views.bench()").find("STOPPED") != std::string::npos);
    page().eval("state.snapshot.bench = null");

    // A result opened: every benchmark with how sure its score is, and what
    // the model said where it was wrong.
    page().eval("state.bench.open = Object.assign(__benchResult('r1', 31, 44), { errors: { truthfulqa: 'HTTP 503: busy' }, cut_off: { gsm8k: 3 }, items: {"
                " mmlu_pro: [{ i: 3, ok: false, read: 'B', answer: 'Answer: B' }, { i: 9, ok: true, read: 'C', answer: 'C' },"
                "            { i: 5, ok: false, read: '', answer: 'and again and again', limit: true, looped: true }],"
                " ifeval: [{ i: 1, ok: false, read: [true, false], answer: 'no' }],"
                " humaneval: [{ i: 2, ok: false, read: 'AssertionError', answer: 'def f(): pass', limit: true }] } })");
    CHECK_EQ(drawn("views.bench()"), "ok");
    const std::string detail = page().eval("views.bench()");
    CHECK(detail.find("data-act=\"bench-close\"") != std::string::npos);
    CHECK(detail.find("read B") != std::string::npos);
    CHECK(detail.find("1 of 2 followed") != std::string::npos);
    CHECK(detail.find("failed") != std::string::npos);
    CHECK(detail.find("hit the limit") != std::string::npos);
    // A reply the runner stopped for going round in circles says that, and
    // not that it ran out of room.
    CHECK(detail.find("<span class=\"part\">#5</span><span class=\"part\">no answer</span><span class=\"part\">looped</span>")
          != std::string::npos);
    CHECK(detail.find("&plusmn;") != std::string::npos);
    CHECK(detail.find("31 of 50") != std::string::npos);
    CHECK(detail.find("HTTP 503: busy") != std::string::npos);   // why a benchmark is missing
    CHECK(detail.find("3 cut off") != std::string::npos);        // answers that ran out of room
    // What it ran on and with.
    CHECK(detail.find("<span>Backend</span><span>Vulkan</span>") != std::string::npos);
    CHECK(detail.find("<span>Writing</span><span>38.1 tok/s</span>") != std::string::npos);
    CHECK(detail.find("data-act=\"bench-post\"") != std::string::npos);
    page().eval("state.bench.open = null; state.bench.adding = false");

    // Posting: exactly what goes, where it goes, and then where it went.
    const std::string posting = "posting: { schema: 1, suite: 'crucible-bench-1', size: 'quick',"
                                " model: { file: 'r1.gguf', sha256: 'ab', repo: 'someone/r1-GGUF' },"
                                " benchmarks: { gsm8k: { n: 50, correct: 44 } } }, public: true";
    for (const std::string& modal : {
             std::string("{ kind: 'bench-post', id: 'r1', posting: null, repo: 'mattsaund/crucible-bench', gh: false }"),
             std::string("{ kind: 'bench-post', id: 'r1', ") + posting + ", repo: 'mattsaund/crucible-bench', gh: true }",
             std::string("{ kind: 'bench-post', id: 'r1', ") + posting + ", repo: 'mattsaund/crucible-bench', gh: true,"
                 " error: 'GitHub said no' }",
             std::string("{ kind: 'bench-post', id: 'r1', ") + posting + ", repo: 'mattsaund/crucible-bench', gh: true,"
                 " posted: true, url: 'https://github.com/mattsaund/crucible-bench/issues/1' }"}) {
        CHECK_EQ(drawn("modalView(" + modal + ")"), "ok");
    }
    const std::string ready = page().eval("modalView({ kind: 'bench-post', id: 'r1', " + posting
                                          + ", repo: 'mattsaund/crucible-bench', gh: true })");
    CHECK(ready.find("crucible-bench-1") != std::string::npos);
    CHECK(ready.find("someone/r1-GGUF") != std::string::npos);
    CHECK(ready.find("Not on Hugging Face") == std::string::npos);
    CHECK_EQ(page().eval("/data-act=\"bench-post-go\"\\s+>/.test(modalView({ kind: 'bench-post', id: 'r1', " + posting
                         + ", repo: 'mattsaund/crucible-bench', gh: true }))"), std::string("true"));
    CHECK(page().eval("modalView({ kind: 'bench-post', id: 'r1', " + posting + ", repo: 'mattsaund/crucible-bench', gh: true,"
                      " posted: true, url: 'https://github.com/mattsaund/crucible-bench/issues/1' })")
              .find("issues/1") != std::string::npos);
    // A model file Hugging Face does not have publicly is not posted: the
    // dialog says so in a word or two, and Post stays off.
    const std::string unknown = "{ kind: 'bench-post', id: 'r1', posting: { schema: 1, model: { file: 'mine.gguf' } },"
                                " public: false, repo: 'mattsaund/crucible-bench', gh: true }";
    CHECK_EQ(drawn("modalView(" + unknown + ")"), "ok");
    CHECK(page().eval("modalView(" + unknown + ")").find("Not on Hugging Face") != std::string::npos);
    CHECK_EQ(page().eval("/data-act=\"bench-post-go\"\\s+disabled/.test(modalView(" + unknown + "))"), std::string("true"));
    const std::string offline = "{ kind: 'bench-post', id: 'r1', posting: { schema: 1, model: { file: 'mine.gguf' } },"
                                " public: false, offline: true, repo: 'mattsaund/crucible-bench', gh: false }";
    CHECK(page().eval("modalView(" + offline + ")").find("Hugging Face unreachable") != std::string::npos);
    CHECK_EQ(page().eval("/data-act=\"bench-post-go\"\\s+disabled/.test(modalView(" + offline + "))"), std::string("true"));
    // Nor a model made here, which nobody is asked about.
    const std::string made = "{ kind: 'bench-post', id: 'r1', posting: { schema: 1, model: { file: 'kitchen.gguf' } },"
                             " public: false, made: 'fine-tune', repo: 'mattsaund/crucible-bench', gh: false }";
    CHECK_EQ(drawn("modalView(" + made + ")"), "ok");
    CHECK(page().eval("modalView(" + made + ")").find("Not public") != std::string::npos);
    CHECK_EQ(page().eval("/data-act=\"bench-post-go\"\\s+disabled/.test(modalView(" + made + "))"), std::string("true"));

    // And it is a tab like the others, a key away.
    CHECK(page().eval("topView()").find("data-view=\"bench\"") != std::string::npos);
    page().eval("state.bench.results = null; state.bench.board = null; state.bench.picked = {}; state.models = window.__modelsWas");
}

/// The board as Crucible reads it: gpt-oss posted twice, quick and then full;
/// LFM2.5, whose file is on this machine; a result of one skill; and two full
/// of markup, one with a repository the Models tab could open. Beside it,
/// what this machine kept: a fine-tune made here, a run that is on the board
/// already, and a run stopped before it finished anything.
const char* const kBoardState = R"JS(
window.__skillsAt = function (v) {
  return { knowledge: v, reasoning: v, math: v, code: v, instructions: v, commonsense: v, truthfulness: v };
};
window.__entry = function (id, repo, file, size, v, more) {
  return Object.assign({ id: id, schema: 1, suite: 'crucible-bench-1', size: size, date: '2026-10-26',
    crucible: '0.9.5', llama: 'b11846',
    model: { file: file, repo: repo, sha256: file + '-sha', bytes: 1246255456, params: 1170340608, quant: 'Q8_0', arch: 'lfm2' },
    settings: { context: 20480, max_tokens: 16384, temperature: 0, effort: 'default' },
    hardware: { backend: 'CUDA', gpu_count: 1, memory_gb: 16.7 },
    benchmarks: { arc: { n: 50, correct: Math.round(v * 50) } }, skills: __skillsAt(v), overall: v,
    speed: { gen_tps: 255.9, prompt_tps: 23313 }, seconds: 766,
    posted: { user: 'poster' + id, issue: id, at: '2026-10-26T16:07:10Z' } }, more || {});
};
window.__modelsWas = state.models;
state.models = { directory: '/m', display: '/m', models: [
  { name: 'LFM2.5-1.2B-Instruct-Q8_0.gguf', path: '/m/LFM2.5-1.2B-Instruct-Q8_0.gguf', bytes: 1246255456, format: 'gguf' },
  { name: 'kitchen-Q4_K_M.gguf', path: '/m/kitchen-Q4_K_M.gguf', bytes: 7e8, format: 'gguf' }] };
state.bench.board = { offline: false, entries: [
  __entry(2, 'ggml-org/gpt-oss-20b-GGUF', 'gpt-oss-20b-MXFP4.gguf', 'quick', 0.81),
  __entry(3, 'ggml-org/gpt-oss-20b-GGUF', 'gpt-oss-20b-MXFP4.gguf', 'full', 0.74),
  __entry(1, 'lmstudio-community/LFM2.5-1.2B-Instruct-GGUF', 'LFM2.5-1.2B-Instruct-Q8_0.gguf', 'quick', 0.62),
  __entry(6, 'evil/ok-GGUF', '<img src=y onerror=alert(3)>.gguf', 'quick', 0.55),
  __entry(5, 'evil/<b>x</b>', '<img src=x onerror=alert(1)>.gguf', 'quick', 0.5, {
    hardware: { backend: '<i>CUDA</i>', gpu_count: 1 }, crucible: '<s>0.9.5</s>', llama: '<em>b1</em>',
    settings: { context: 20480, max_tokens: 16384, temperature: 0, effort: '<u>high</u>' },
    speed: { gen_tps: '<b>9</b>' }, posted: { user: '<script>alert(2)</script>' } }),
  __entry(4, 'someone/partial-GGUF', 'partial.gguf', 'quick', 0.9, { skills: { reasoning: 0.9 }, overall: 0.9 })] };
state.bench.board.entries[4].model.quant = '<q>Q8</q>';
state.bench.board.entries[4].model.arch = '<a href=x>';
state.bench.results = { results: [
  Object.assign(__entry('mine-1', '', 'kitchen-Q4_K_M.gguf', 'quick', 0.7),
                { posted: null, made: 'fine-tune', path: '/m/kitchen-Q4_K_M.gguf' }),
  Object.assign(__entry('mine-2', '', 'LFM2.5-1.2B-Instruct-Q8_0.gguf', 'quick', 0.62), { posted: null }),
  Object.assign(__entry('mine-3', '', 'stopped.gguf', 'quick', 0), { posted: null, benchmarks: {}, skills: {},
                                                                    overall: null, stopped: true })] };
state.bench.compare = [];
state.bench.seeded = false;
'ok'
)JS";

TEST(the_bench_tab_opens_on_the_board) {
    CHECK_EQ(page().eval(kBoardState), "ok");
    CHECK_EQ(drawn("views.bench()"), "ok");
    const std::string home = page().eval("views.bench()");
    // The board first, with the way to add a benchmark in the corner and
    // nothing of a run's setup.
    CHECK(home.find("data-act=\"bench-add\"") != std::string::npos);
    CHECK(home.find("data-act=\"bench-start\"") == std::string::npos);
    CHECK(home.find("TOP MODELS") != std::string::npos);
    CHECK(home.find("COMPARE") != std::string::npos);
    CHECK(home.find("RESULTS") != std::string::npos);
    // Each of the best with its maker's logo, or the square that waits for one.
    CHECK(home.find("class=\"order-row\">\n          <span class=\"order-n\">1</span>\n          <span class=\"logo\"") != std::string::npos
          || home.find("<img class=\"logo") != std::string::npos);

    // The best models, each once and by its widest run of every skill: the
    // full gpt-oss run, not its better quick one, and nothing of one skill
    // or from a repository the Models tab could not open.
    CHECK_EQ(page().eval("benchBest(benchEntries()).map(function (b) { return b.entry.id; }).join(' ')"),
             std::string("3 1 6"));
    CHECK(home.find("<span class=\"order-n\">1</span>") != std::string::npos);
    CHECK(home.find("<span class=\"bench-overall\">74.0</span>") != std::string::npos);
    // Sizes as people say them: 999,885,952 weights make a 1B model.
    CHECK_EQ(page().eval("[paramsLabel(0.999885952), paramsLabel(0.6), paramsLabel(1.17034)].join(' ')"),
             std::string("1B 600M 1.2B"));
    // One not here is a click from its download; one here says so.
    CHECK(home.find("data-act=\"bench-get\" data-id=\"3\"") != std::string::npos);
    CHECK(home.find("data-act=\"bench-get\" data-id=\"1\"") == std::string::npos);
    CHECK(home.find("<span class=\"status here\">here</span>") != std::string::npos);

    // Every result to browse, this machine's among them: the board's order,
    // a run already on the board once, and nothing of a run that finished
    // nothing.
    CHECK_EQ(page().eval("benchDirectory().map(function (x) { return x.key; }).join(' ')"),
             std::string("board-2 board-3 mine-1 board-1 board-6 board-5 board-4"));
    CHECK_EQ(page().eval("/data-act=\"bench-detail\"\\s+data-id=\"mine-1\"/.test(views.bench())"), std::string("true"));
    CHECK(home.find("data-act=\"bench-compare\" data-id=\"mine-1\"") != std::string::npos);
    CHECK(home.find("<td>you</td>") != std::string::npos);
    CHECK(home.find("<span class=\"status\">fine-tune</span>") != std::string::npos);
    CHECK(home.find("data-id=\"mine-2\"") == std::string::npos);
    CHECK(home.find("data-id=\"mine-3\"") == std::string::npos);
    CHECK(home.find("6 results") != std::string::npos);

    // What other people posted is text, never markup, wherever it is drawn.
    for (const char* raw : {"<img src=", "<script", "<b>x", "<i>CUDA", "<q>", "<a href=x"}) {
        CHECK(home.find(raw) == std::string::npos);
    }
    CHECK(home.find("&lt;img src=y onerror=alert(3)&gt;") != std::string::npos);   // a top model's name
    CHECK(home.find("&lt;script&gt;alert(2)&lt;/script&gt;") != std::string::npos);   // who posted it

    // Searched by any of its names, by who posted it, or "you".
    for (const auto& [query, wanted] : std::vector<std::pair<std::string, std::string>>{
             {"you", "mine-1"}, {"poster3", "board-3"}, {"ggml-org", "board-2 board-3"}, {"LFM2.5", "board-1"}}) {
        CHECK_EQ(page().eval("(function () { var q = " + lit(query) + "; return benchDirectory().filter(function (x) {"
                             " return benchSought(x).includes(q.toLowerCase()); }).map(function (x) { return x.key; }).join(' '); })()"),
                 wanted);
    }
    page().eval("state.bench.boardQuery = 'nothing like it'");
    CHECK(page().eval("views.bench()").find("Nothing matches.") != std::string::npos);
    page().eval("state.bench.boardQuery = ''");

    // Offline: the directory says so in a word, and what is kept here still shows.
    page().eval("window.__boardWas = state.bench.board; state.bench.board = { entries: [], offline: true }");
    CHECK_EQ(drawn("views.bench()"), "ok");
    const std::string offline = page().eval("views.bench()");
    CHECK(offline.find("<span class=\"status\">offline</span>") != std::string::npos);
    CHECK(offline.find("data-id=\"mine-1\"") != std::string::npos);
    CHECK(offline.find("TOP MODELS") == std::string::npos);
    page().eval("window.__resultsWas = state.bench.results; state.bench.results = { results: [] }");
    CHECK(page().eval("views.bench()").find("Board offline.") != std::string::npos);
    page().eval("state.bench.results = __resultsWas; state.bench.board = Object.assign({}, __boardWas, { offline: true })");
    CHECK(page().eval("views.bench()").find("offline copy") != std::string::npos);
    page().eval("state.bench.board = __boardWas");

    // The chart from the start: the board's best three, once.
    page().eval("var was = render; render = function () {}; benchSeed(); render = was");
    CHECK_EQ(page().eval("state.bench.compare.map(function (c) { return c.key; }).join(' ')"),
             std::string("board-3 board-1 board-6"));
    page().eval("state.bench.compare = []; benchSeed()");
    CHECK_EQ(page().eval("state.bench.compare.length"), std::string("0"));

    // A board entry opened: its benchmarks, what it ran on and with, the
    // chart, and its download; every string of it text.
    page().eval("actions['bench-open-board']({ dataset: { id: '3' } })");
    CHECK_EQ(page().eval("state.bench.open.board"), std::string("true"));
    CHECK_EQ(drawn("views.bench()"), "ok");
    const std::string entry = page().eval("views.bench()");
    CHECK(entry.find("data-act=\"bench-close\"") != std::string::npos);
    CHECK(entry.find("<span class=\"caption\">REPO</span> ggml-org/gpt-oss-20b-GGUF") != std::string::npos);
    CHECK(entry.find("37 of 50") != std::string::npos);
    CHECK(entry.find("<svg class=\"radar\"") != std::string::npos);
    CHECK(entry.find("<span>GPU memory</span><span>16.7 GB</span>") != std::string::npos);
    CHECK(entry.find("<span>Context</span><span>20480 tokens</span>") != std::string::npos);
    CHECK(entry.find("<span>Took</span><span>13 min</span>") != std::string::npos);
    CHECK(entry.find("data-act=\"bench-get\" data-id=\"3\"") != std::string::npos);
    CHECK(entry.find("data-act=\"bench-post\"") == std::string::npos);
    page().eval("actions['bench-open-board']({ dataset: { id: '5' } })");
    CHECK_EQ(drawn("views.bench()"), "ok");
    const std::string evil = page().eval("views.bench()");
    for (const char* raw : {"<img src=", "<script", "<b>", "<i>", "<s>", "<em>", "<u>", "<q>", "<a href"}) {
        CHECK(evil.find(raw) == std::string::npos);
    }
    CHECK(evil.find("&lt;i&gt;CUDA&lt;/i&gt;") != std::string::npos);
    CHECK(evil.find("&lt;u&gt;high&lt;/u&gt;") != std::string::npos);
    CHECK(evil.find("<span>Writing</span>") == std::string::npos);   // a speed that is not a number
    CHECK(evil.find("data-act=\"bench-get\"") == std::string::npos);  // no repository to open
    page().eval("actions['bench-close']()");
    CHECK(page().eval("views.bench()").find("TOP MODELS") != std::string::npos);

    // This machine's fine-tune is no file the board takes, and its post
    // control says so rather than offering a post that would be refused.
    page().eval("actions['bench-add']()");
    const std::string mine = page().eval("views.bench()");
    CHECK(mine.find("<button class=\"action small\" disabled>Not public</button>") != std::string::npos);
    CHECK(mine.find("data-act=\"bench-post\" data-id=\"mine-1\"") == std::string::npos);
    CHECK(mine.find("data-act=\"bench-post\" data-id=\"mine-2\"") != std::string::npos);
    page().eval("actions['bench-home'](); state.bench.results = null; state.bench.board = null; state.bench.compare = [];"
                " state.models = window.__modelsWas");
}

TEST(a_board_entry_downloads_through_the_models_tab) {
    CHECK_EQ(page().eval(kBoardState), "ok");
    // Download goes to the Models tab, searching for the repository and open
    // on it: the tab's own download, not a second one.
    page().eval("window.__asked = []; window.__callWas = call;"
                "call = function (method, params) { __asked.push(method + ' ' + JSON.stringify(params || {}));"
                "  return new Promise(function () {}); };"
                "actions['bench-get']({ dataset: { id: '3' } }); call = __callWas");
    CHECK_EQ(page().eval("state.view"), std::string("models"));
    CHECK_EQ(page().eval("market().query + ' ' + market().format + ' ' + market().selected"),
             std::string("ggml-org/gpt-oss-20b-GGUF gguf ggml-org/gpt-oss-20b-GGUF"));
    CHECK(page().eval("__asked.join('\\n')").find("market.search {\"query\":\"ggml-org/gpt-oss-20b-GGUF\"") != std::string::npos);
    CHECK(page().eval("__asked.join('\\n')").find("market.model {\"id\":\"ggml-org/gpt-oss-20b-GGUF\"") != std::string::npos);
    // With the file the result was of chosen, not the suggestion.
    page().eval("call = function (method) { return Promise.resolve(method === 'market.model'"
                "  ? { id: 'ggml-org/gpt-oss-20b-GGUF', format: 'gguf', options: ["
                "      { name: 'Q4_K_M', repo: 'ggml-org/gpt-oss-20b-GGUF', names: ['gpt-oss-20b-Q4_K_M.gguf'], recommended: true },"
                "      { name: 'MXFP4', repo: 'ggml-org/gpt-oss-20b-GGUF', names: ['gpt-oss-20b-MXFP4.gguf'] }] }"
                "  : method === 'market.readme' ? { text: '' } : { listings: [], next: '' }); };"
                "marketOpen('ggml-org/gpt-oss-20b-GGUF', 'gpt-oss-20b-MXFP4.gguf'); call = __callWas");
    CHECK_EQ(page().eval("market().option"), std::string("MXFP4"));
    page().eval("call = function (method) { return Promise.resolve(method === 'market.model'"
                "  ? { id: 'a/b', format: 'gguf', options: [{ name: 'Q4_K_M', repo: 'a/b', names: ['b-Q4_K_M.gguf'], recommended: true }] }"
                "  : { text: '' }); }; marketOpen('a/b'); call = __callWas");
    CHECK_EQ(page().eval("market().option"), std::string("Q4_K_M"));
    page().eval("state.view = 'chat'; market().selected = ''; market().detail = null; market().query = '';"
                " market().results = null; state.bench.results = null; state.bench.board = null;"
                " state.bench.compare = []; state.models = window.__modelsWas");
}

TEST(a_model_made_in_create_is_benched_against_its_base) {
    // A fine-tune of unsloth/Llama-3.2-1B at Q4_K_M, and GGUFs of that base
    // here at two sizes, one named by a converter that puts the author first.
    const std::string fixture =
        "window.__recipesWas = state.recipes; window.__modelsWas2 = state.models;"
        "state.models = { directory: '/m', display: '/m', models: ["
        "  { name: 'kitchen-physicist-Q4_K_M.gguf', path: '/m/kitchen-physicist-Q4_K_M.gguf', bytes: 9e8, format: 'gguf' },"
        "  { name: 'unsloth/Llama-3.2-1B-GGUF/Llama-3.2-1B-Q8_0.gguf', path: '/m/u/Llama-3.2-1B-Q8_0.gguf', bytes: 1.3e9, format: 'gguf' },"
        "  { name: 'bartowski/x/unsloth_Llama-3.2-1B-Q4_K_M.gguf', path: '/m/b/unsloth_Llama-3.2-1B-Q4_K_M.gguf', bytes: 8e8, format: 'gguf' },"
        "  { name: 'Llama-3.2-1B-Instruct-Q4_K_M.gguf', path: '/m/Llama-3.2-1B-Instruct-Q4_K_M.gguf', bytes: 8e8, format: 'gguf' }] };"
        "state.recipes = { memory: 6e9, recipes: [aRecipe('finished', { trained_path: '/m/kitchen-physicist-Q4_K_M.gguf',"
        "  trained_there: true })] };"
        "state.open.recipe = 'kitchen-finished'; 'ok'";
    CHECK_EQ(page().eval(fixture), "ok");
    CHECK_EQ(drawn("views.create()"), "ok");
    // Its base at its own quantization, by either name, and never another model.
    CHECK_EQ(page().eval("baseGguf(state.recipes.recipes[0])"), std::string("/m/b/unsloth_Llama-3.2-1B-Q4_K_M.gguf"));
    CHECK(page().eval("views.create()").find("data-act=\"recipe-bench\" data-id=\"kitchen-finished\">Bench vs base")
          != std::string::npos);
    page().eval("state.models.models.splice(2, 1)");
    CHECK_EQ(page().eval("baseGguf(state.recipes.recipes[0])"), std::string("/m/u/Llama-3.2-1B-Q8_0.gguf"));
    CHECK_EQ(page().eval("JSON.stringify(ggufParts('a/Big-UD-Q8_0-00001-of-00002.gguf'))"),
             std::string("{\"stem\":\"Big-UD-Q8_0\",\"model\":\"Big\",\"quant\":\"UD-Q8_0\"}"));

    // Pressed: both run at the size and effort set on Bench, on its add
    // screen, with the run under way.
    page().eval("window.__asked = []; window.__callWas = call;"
                "call = function (method, params) { __asked.push([method, params]);"
                "  return method === 'bench.start' ? new Promise(function () {}) : Promise.reject(new Error('not here')); };"
                "state.bench.size = 'standard'; state.bench.effort = 'low';"
                "actions['recipe-bench']({ dataset: { id: 'kitchen-finished' } }); call = __callWas");
    CHECK_EQ(page().eval("state.view + ' ' + state.bench.adding"), std::string("bench true"));
    CHECK_EQ(page().eval("JSON.stringify(__asked.filter(function (a) { return a[0] === 'bench.start'; })[0][1])"),
             std::string("{\"models\":[{\"path\":\"/m/kitchen-physicist-Q4_K_M.gguf\",\"name\":\"kitchen-physicist-Q4_K_M.gguf\"},"
                         "{\"path\":\"/m/u/Llama-3.2-1B-Q8_0.gguf\",\"name\":\"Llama-3.2-1B-Q8_0.gguf\"}],"
                         "\"size\":\"standard\",\"effort\":\"low\",\"benchmarks\":[\"mmlu_pro\",\"arc\",\"gsm8k\",\"math500\","
                         "\"humaneval\",\"mbpp\",\"ifeval\",\"hellaswag\",\"truthfulqa\"]}"));
    CHECK_EQ(page().eval("Object.keys(state.bench.picked).join(' ')"),
             std::string("/m/kitchen-physicist-Q4_K_M.gguf /m/u/Llama-3.2-1B-Q8_0.gguf"));

    // When the run ends, the two go on the chart in place of what was there.
    page().eval("state.bench.compare = [{ key: 'board-9', color: '#3987e5', entry: {}, board: true }];"
                "window.__benchHook = onSnapshot.find(function (job) { return String(job).indexOf('benchWasRunning') >= 0; });"
                "window.__versusResult = function (id, path) { return { id: id, size: 'standard', path: path,"
                "  model: { file: path.split('/').pop() }, benchmarks: { arc: { n: 200, correct: 150 } }, skills: { reasoning: 0.75 } }; };"
                "call = function (method) { return method === 'bench.results' ? Promise.resolve({ results: ["
                "  __versusResult('v-1', '/m/u/Llama-3.2-1B-Q8_0.gguf'), __versusResult('v-0', '/m/kitchen-physicist-Q4_K_M.gguf'),"
                "  __versusResult('old-0', '/m/old.gguf')] }) : Promise.reject(new Error('no')); };"
                "window.__renderWas = render; render = function () {};"
                "window.__snapshotWas = state.snapshot;"
                "state.snapshot = Object.assign({}, __snapshotWas, { bench: { running: true, models: [] } }); __benchHook();"
                "state.snapshot = Object.assign({}, __snapshotWas, { bench: { running: false, models: ["
                "  { path: '/m/kitchen-physicist-Q4_K_M.gguf', result: 'v-0' }, { path: '/m/u/Llama-3.2-1B-Q8_0.gguf', result: 'v-1' }] } });"
                "__benchHook(); 'ok'");
    CHECK_EQ(page().eval("state.bench.compare.map(function (c) { return c.key; }).join(' ')"), std::string("v-0 v-1"));
    CHECK_EQ(page().eval("String(state.bench.versus)"), std::string("null"));
    page().eval("call = __callWas; render = __renderWas; state.snapshot = __snapshotWas");

    // Without a GGUF of the base here, the Models tab looks for one.
    page().eval("state.view = 'create'; state.models.models = state.models.models.slice(0, 1)");
    CHECK(page().eval("views.create()").find("data-act=\"recipe-base\" data-id=\"kitchen-finished\"") != std::string::npos);
    page().eval("call = function () { return new Promise(function () {}); };"
                "actions['recipe-base']({ dataset: { id: 'kitchen-finished' } }); call = __callWas");
    CHECK_EQ(page().eval("state.view + ' ' + market().query"), std::string("models Llama-3.2-1B"));

    // A condensed copy is benched against the file it was condensed from; an
    // export that is not a GGUF is no model Bench can run.
    page().eval("state.models.models.push({ name: 'flint-Q6_K.gguf', path: '/m/flint-Q6_K.gguf', bytes: 3e10, format: 'gguf' });"
                "state.recipes = { memory: 6e9, recipes: [aRecipe('finished', { id: 'flint-q4', method: 'condense',"
                "  base: { source: 'local', id: '/m/flint-Q6_K.gguf', path: '/m/flint-Q6_K.gguf', label: 'flint-Q6_K.gguf' },"
                "  trained_path: '/m/flint Q4_K_M.gguf', trained_there: true, data: [], source_quantization: 'Q6_K' }),"
                "  aRecipe('finished', { id: 'mlx-one', format: 'mlx', trained_path: '/m/mlx-one', trained_there: true })] };"
                "state.open.recipe = 'flint-q4'");
    CHECK_EQ(drawn("views.create()"), "ok");
    CHECK(page().eval("views.create()").find("data-act=\"recipe-bench\" data-id=\"flint-q4\"") != std::string::npos);
    CHECK_EQ(page().eval("baseGguf(state.recipes.recipes[0])"), std::string("/m/flint-Q6_K.gguf"));
    page().eval("state.open.recipe = 'mlx-one'");
    CHECK(page().eval("views.create()").find("recipe-bench") == std::string::npos);
    CHECK(page().eval("views.create()").find("recipe-base") == std::string::npos);

    page().eval("state.view = 'chat'; state.open.recipe = null; state.recipes = __recipesWas; state.models = __modelsWas2;"
                " state.bench.adding = false; state.bench.picked = {}; state.bench.size = 'quick'; state.bench.effort = '';"
                " state.bench.compare = []; state.bench.results = null; state.bench.board = null; state.error = '';"
                " market().query = ''; market().results = null");
}

TEST(no_two_page_scripts_define_the_same_name) {
    // Every script on the page shares one scope, so a function a later file
    // declares replaces an earlier one of the same name without a word:
    // models.js's ago(iso) blanked every "Started" in Create for a release.
    // Crucible's own scripts only; the vendored ones are minified.
    std::map<std::string, int> seen;
    std::vector<std::string>   twice;
    for (const std::string& script : page_scripts()) {
        if (script.find("SPDX-License-Identifier: MIT") == std::string::npos) {
            continue;
        }
        std::size_t start = 0;
        while (start < script.size()) {
            std::size_t end = script.find('\n', start);
            if (end == std::string::npos) {
                end = script.size();
            }
            std::string line = script.substr(start, end - start);
            start = end + 1;
            for (const char* lead : {"async function ", "function ", "const ", "let ", "class "}) {
                if (line.rfind(lead, 0) != 0) {
                    continue;
                }
                std::string name;
                for (std::size_t i = std::strlen(lead); i < line.size(); ++i) {
                    const char c = line[i];
                    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_' && c != '$') {
                        break;
                    }
                    name += c;
                }
                if (!name.empty() && ++seen[name] == 2) {
                    twice.push_back(name);
                }
                break;
            }
        }
    }
    for (const std::string& name : twice) {
        std::printf("      defined twice: %s\n", name.c_str());
    }
    CHECK(twice.empty());
    // And it looked: names from the first script and the last.
    CHECK(seen.count("render") == 1);
    CHECK(seen.count("benchSkills") == 1);
    CHECK(seen.size() > 200);
}

TEST(the_page_puts_back_the_turns_it_was_sent_as_stubs) {
    // A push with every turn whole, then one with the first as a stub: the
    // page puts the kept turn back.
    CHECK_EQ(page().eval(R"JS((function () {
      var was = state.snapshot, keepRender = render; render = function () {};
      var one = { prompt: 'a', reply: 'b', streaming: false, rev: 5 };
      window.crucibleSnapshot(Object.assign({}, was, { turns: [one] }));
      window.crucibleSnapshot(Object.assign({}, was, { turns: [{ rev: 5, stub: true },
        { prompt: 'c', reply: 'd', streaming: true, rev: 6 }] }));
      var out = state.snapshot.turns.map(function (t) { return t.prompt; }).join(',');
      state.snapshot = was; render = keepRender;
      return out; })())JS"), std::string("a,c"));

    // A stub for a turn the page never had: the transcript stays as it was,
    // and the whole snapshot is asked for.
    CHECK_EQ(page().eval(R"JS((function () {
      var was = state.snapshot, keepRender = render, keepCall = call, asked = [];
      render = function () {};
      call = function (method) { asked.push(method); return new Promise(function () {}); };
      window.crucibleSnapshot(Object.assign({}, was, { turns: [{ prompt: 'x', reply: 'y', rev: 40 }] }));
      window.crucibleSnapshot(Object.assign({}, was, { turns: [{ rev: 99, stub: true }] }));
      var out = state.snapshot.turns.map(function (t) { return t.prompt; }).join(',') + '|'
              + (asked.indexOf('snapshot') >= 0);
      state.snapshot = was; render = keepRender; call = keepCall;
      return out; })())JS"), std::string("x|true"));

    // A finished turn already on screen is drawn as an empty stub with its
    // signature; one that is not, whole.
    CHECK_EQ(page().eval(R"JS((function () {
      var turns = [{ prompt: 'q', reply: 'r', streaming: false, rev: 7, actions: [] }];
      var whole = turnsView(turns, 0);
      var sig = /data-sig="([^"]*)"/.exec(whole)[1];
      turnsShown.transcript.add('t0|' + sig.replace(/&#39;/g, "'").replace(/&quot;/g, '"').replace(/&amp;/g, '&'));
      var stub = turnsView(turns, 0);
      turnsShown.transcript.clear();
      return (whole.indexOf('data-stub') < 0) + ',' + (stub.indexOf('data-stub') >= 0) + ',' + (stub.indexOf('>r<') < 0);
    })())JS"), std::string("true,true,true"));
}
