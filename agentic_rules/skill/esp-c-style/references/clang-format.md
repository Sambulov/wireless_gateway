# clang-format для этого проекта

Источники:
- Правила ядра: https://www.kernel.org/doc/html/latest/process/coding-style.html
- Документация по clang-format в ядре: https://www.kernel.org/doc/html/latest/dev-tools/clang-format.html
- Оригинальный конфиг ядра: https://github.com/torvalds/linux/blob/master/.clang-format

Конфиг ниже — адаптация kernel-конфига под этот проект (4 пробела вместо табов, скобка функции на той же строке, `_t` не трогаем). Сохрани как `.clang-format` в корне репозитория или используй через `--style=file:<path>`.

```yaml
BasedOnStyle: LLVM
IndentWidth: 4
UseTab: Never
ColumnLimit: 80
BreakBeforeBraces: Attach          # K&R, включая функции — как в проекте
AllowShortBlocksOnASingleLine: Never
AllowShortFunctionsOnASingleLine: None
AllowShortIfStatementsOnASingleLine: Never
AllowShortLoopsOnASingleLine: false
PointerAlignment: Right            # char *buf; — * прижимается к имени
SpaceAfterCStyleCast: false
SpaceBeforeParens: ControlStatements
SortIncludes: false                # порядок инклюдов в проекте задан вручную
AlignConsecutiveAssignments: false
AlignConsecutiveDeclarations: false
AlignConsecutiveMacros: false
ReflowComments: false              # не переливать комментарии — они выровнены вручную
BinPackArguments: false
BinPackParameters: false
```

## Использование

```bash
# просмотр диффа того, что бы изменил clang-format (для ревью патчей):
git clang-format --diff main/...          # или clang-format --dry-run -Werror file.c
# форматирование нового файла:
clang-format -i path/to/new_file.c
```

## Чего clang-format НЕ умеет (из документации ядра — проверять глазами)

- Выравнивание однострочных `#define` в столбик — сбросит в одиночный пробел.
- Выравнивание designated initializers (`.owner = ...`) — сбросит.
- Многострочные макросы с `\` — может переформатировать неоптимально; после прогона поправить вручную.

## Если нужно строже/шире

Дополнительные опции из kernel-документации, включаемые точечно (не в общий конфиг, чтобы диффы оставались маленькими):
- `AlignConsecutiveAssignments` / `AlignConsecutiveDeclarations` — выравнивание столбцов.
- `SortIncludes: true` — сортировка #include.
- `ReflowComments: true` — переформатирование текста комментариев.

Версия clang-format должна быть не ниже 14 (современные правила; в ядре периодически поднимают минимальную версию).
