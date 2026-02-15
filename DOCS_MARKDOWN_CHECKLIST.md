# Documentation Markdown Checklist

Use this checklist before pushing documentation updates.

## Beginner Clarity

- Explain hardware terms in plain language first.
- Keep steps short and ordered.
- Include expected output examples where possible.
- Keep naming consistent with firmware/UI labels.

## Markdown Rendering (GitHub)

- Use one `#` title per file.
- Keep heading levels ordered (`##` then `###`).
- Use fenced code blocks with language labels (for example `bash`, `c`, `txt`).
- Keep list indentation consistent (2 or 4 spaces for nested items).
- Avoid raw tabs in markdown source.
- Avoid trailing spaces unless intentional.

## Tables and Diagrams

- Keep table header separators valid (`|---|---|`).
- Use monospace formatting for pins, addresses, registers, and commands.
- Keep ASCII diagrams in fenced `txt` blocks.
- Verify diagrams still align in GitHub preview.

## Terminology Consistency

- Use `stage-2`, `stage-3`, and `stage-4` consistently.
- Keep unknown-word label spelling aligned with runtime data files: `UnRecognisedXX`.
- Keep menu names aligned with LCD text (`User Menu`, `Main Menu Pg 0/1`).

## File-Format Accuracy

- `Language.dat`: `HH LanguageName` + CRLF.
- `Dictionary.dat`: 73-byte fixed-width text records.
- `NewWords.dat`: same 73-byte format as `Dictionary.dat`.

## Quick Validation

1. Open preview in GitHub/VS Code Markdown preview.
2. Confirm lists and tables render correctly.
3. Confirm code blocks are syntax-highlighted.
4. Confirm links resolve.
5. Confirm examples match current firmware behavior.
