# Attaching an AI

The tool ships no model and chooses none for you. You attach whichever one you
already use, or one running on your own machine, and it answers questions about
the project you have open.

Nothing is attached by default. Everything in the Script tab works without one.

## The short version

In the Unreal console (the ` key, or Window > Developer Tools > Output Log):

```
BF6.Assist.Endpoint http://127.0.0.1:11434/v1/chat/completions
BF6.Assist.Model    llama3.1
BF6.Assist.Test
```

That is a model running on your own machine and needs no key. For a hosted one,
see the table below and add a key file.

`BF6.Assist.Setup` prints these steps in the editor. The Script tab's **Ask the
AI** column prints them too, with a button to copy them.

## The three settings

| Command | What it is |
|---|---|
| `BF6.Assist.Endpoint <url>` | Where questions are sent. |
| `BF6.Assist.Model <name>` | Which model to ask for. |
| `BF6.Assist.KeyFile <path>` | A file **of yours** holding the key on one line. |

The endpoint and the model are saved with the project's editor settings and
survive a restart. Changing them takes effect immediately: there is nothing to
restart.

### Endpoints

| Provider | Endpoint | Model examples |
|---|---|---|
| On your machine | `http://127.0.0.1:11434/v1/chat/completions` | whatever Ollama or LM Studio is serving |
| OpenAI | `https://api.openai.com/v1/chat/completions` | `gpt-4o-mini` |
| Anthropic | `https://api.anthropic.com/v1/messages` | `claude-sonnet-4-5` |

Anything that speaks the OpenAI chat-completions shape will work. Both the
OpenAI and the Anthropic response shapes are understood.

## The key

**The tool never stores your key.** Not in its settings, not in a file it owns,
not in the log, and never in the editor page - that page is a `file://`
document, and a key that reached it would effectively be published.

There are two ways to give it one, and neither ends with a credential inside the
tool:

1. **A file of your own.** Put the key on one line in a text file anywhere you
   like, then `BF6.Assist.KeyFile C:\path\to\that\file.txt`. The tool remembers
   the **path**, reads the file at the moment of a request, and drops the
   contents again. Move or delete the file and the key is gone.
2. **An environment variable.** Set `BF6_AI_KEY` before starting the editor.
   This takes precedence over the key file. `BF6_AI_ENDPOINT` and `BF6_AI_MODEL`
   work the same way and override the saved settings, which is useful on a build
   machine.

`BF6.Assist.KeyFile` with no argument clears it.

## Where your questions go

`BF6.Assist.Status` says, every time, in plain words:

```
BF6 Assist
  linked        : yes
  endpoint      : https://api.openai.com/v1/chat/completions
  model         : gpt-4o-mini
  key           : present (never stored, logged or sent to a page)
  where it goes : OFF THIS MACHINE - the briefing includes your project
```

A loopback endpoint - `127.0.0.1`, `localhost`, `::1` - is the only case where
the tool will tell you nothing leaves the machine, and it decides that from the
URL's actual host rather than from the text of it. Anything else says plainly
that your script is going with the question.

The Ask box says the same thing before you press Ask, along with the size of
what is about to be sent.

## What a question carries

The briefing is built for the project you have open, and it is prose rather than
a data dump. It contains:

- the Portal rules that decide whether generated code actually works;
- every `mod.*` command and every event, with parameter names;
- the file you are looking at, and what you selected in it;
- the other files open in the project, and the names of the ones that are not;
- the text your mod shows players, and the settings on your Values list;
- what is selected in the Unreal scene, and the exact call that reaches it;
- what the type checker is reporting, what Portal said about your last upload,
  and the tail of the game log;
- the conversation so far, so a follow-up is a follow-up.

On a 7,900 line mod that comes to about 122 KB. Nothing about your machine goes
with it: no paths, no environment variables, no credentials.

## Using it

Select some code in the Script tab, right-click, and choose **Ask the AI about
this...** (or Ctrl+Shift+A). Type what you want checked, explained or changed.

The answer appears in the **Ask the AI** column beside the code, with the
question above it, and stays there for the session. Code comes back in blocks
with **Replace what I selected**, **Put it at the cursor** and **Copy**.

Nothing is ever applied on its own. Applying is one undoable edit: Ctrl+Z takes
it straight back out. An answer will refuse to apply if the lines have changed
since you asked, if you have opened a different file, or if you have switched to
a different project.

## With nothing attached

**Copy the briefing** puts the whole thing on the clipboard - the rules, the
vocabulary, your project and your question - for pasting into whatever you
already talk to. That is what the briefing was designed for, and it works
whether or not anything is linked.
