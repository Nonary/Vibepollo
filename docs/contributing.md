# Contributing
Read our contribution guide in our organization level
[docs](https://docs.lizardbyte.dev/latest/developers/contributing.html).

## Recommended Tools

| Tool                                                                                                                                                                           | Description                                                             |
|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------|
| <a href="https://www.jetbrains.com/clion/"><img src="https://resources.jetbrains.com/storage/products/company/brand/logos/CLion_icon.svg" width="30" height="30"></a><br>CLion | Recommended IDE for C and C++ development. Free for non-commercial use. |

## Project Patterns

### Browser interface
The browser interface lives under `src_assets/common/assets/web`. It uses Vue,
Pinia, Vue Router, and project-owned components and design tokens; do not add a
third-party component framework. Keep common controls in `components/ui`, use
the semantic tokens generated from `design/tokens.json`, and preserve the
task-first information hierarchy documented in `docs/design-principles.md`.

### Localization
Sunshine and related LizardByte projects are being localized into various languages.
The default language is `en` (English).

![](https://app.lizardbyte.dev/dashboard/crowdin/LizardByte_graph.svg)

@admonition{Community | We are looking for language coordinators to help approve translations.
The goal is to have the bars above filled with green!
If you are interesting, please reach out to us on our Discord server.}

#### CrowdIn
The translations occur on [CrowdIn][crowdin-url].
Anyone is free to contribute to the localization there.

##### Translation Basics
* The brand names *LizardByte* and *Sunshine* should never be translated.
* Other brand names should never be translated. Examples include *AMD*, *Intel*, and *NVIDIA*.

##### CrowdIn Integration
How does it work?

When a change is made to Sunshine source code, a workflow generates new translation templates
that get pushed to CrowdIn automatically.

When translations are updated on CrowdIn, a push gets made to the *l10n_master* branch and a PR is made against the
*master* branch. Once the PR is merged, all updated translations are part of the project and will be included in the
next release.

#### Extraction

##### Locale catalogs

Add or update English locale entries in
`src_assets/common/assets/web/public/assets/locale/en.json` when extending an
established catalog namespace. New interface copy belongs in
`src_assets/common/assets/web/public/assets/locale/ui/en.json` under the `ui`
namespace. Keep JSON keys sorted alphabetically and reuse an established key
when its wording and meaning are an exact match.

> [!IMPORTANT]
> For normal source changes, modify only the applicable English source catalog,
> not translated locale files. The mappings in `crowdin.yml` publish both
> English catalogs; translations are contributed through [CrowdIn][crowdin-url]
> and returned in a localization pull request. Components must use locale keys
> for all user-facing copy rather than embedding English fallback text.

##### C++

There should be minimal cases where strings need to be extracted from C++ source code; however it may be necessary in
some situations. For example the system tray icon could be localized as it is user interfacing.

* Wrap the string to be extracted in a function as shown.
  ```cpp
  #include <boost/locale.hpp>
  #include <string>

  std::string msg = boost::locale::translate("Hello world!");
  ```

> [!TIP]
> More examples can be found in the documentation for
> [boost locale](https://www.boost.org/doc/libs/1_70_0/libs/locale/doc/html/messages_formatting.html).

> [!WARNING]
> The below is for information only. Contributors should never include manually updated template files, or
> manually compiled language files in Pull Requests.

Strings are automatically extracted from the code to the `locale/sunshine.po` template file. The generated file is
used by CrowdIn to generate language specific template files. The file is generated using the
`.github/workflows/localize.yml` workflow and is run on any push event into the `master` branch. Jobs are only run if
any of the following paths are modified.

```yaml
- 'src/**'
```

When testing locally, it may be desirable to manually extract, initialize, update, and compile strings. Python is
required for this, along with the python dependencies in the `./pyproject.toml` file. You can install this with
the following command.

```bash
python -m pip install ".[locale]"
```

Additionally, [xgettext](https://www.gnu.org/software/gettext) must be installed.

* Extract, initialize, and update
  ```bash
  python ./scripts/_locale.py --extract --init --update
  ```

* Compile
  ```bash
  python ./scripts/_locale.py --compile
  ```

> [!IMPORTANT]
> Due to the integration with CrowdIn, it is important to not include any extracted or compiled files in
> Pull Requests. The files are automatically generated and updated by the workflow. Once the PR is merged, the
> translations can take place on [CrowdIn][crowdin-url]. Once the translations are
> complete, a PR will be made to merge the translations into Sunshine.

### Testing

#### Clang Format
Source code is tested against the `.clang-format` file for linting errors. The workflow file responsible for clang
format testing is `.github/workflows/cpp-clang-format-lint.yml`.

Option 1:
```bash
find ./ -iname *.cpp -o -iname *.h -iname *.m -iname *.mm | xargs clang-format -i
```

Option 2 (will modify files):
```bash
python ./scripts/update_clang_format.py
```

#### Unit Testing
Unit tests are enabled for every project build. For a bug fix, first demonstrate the failure with a focused test,
then make that same test pass with the correction. Run the narrowest relevant test target during local iteration.

Even if your changes cannot be covered in the CI, we still encourage you to write the tests for them. This will allow
maintainers to run the tests locally.

[crowdin-url]: https://translate.lizardbyte.dev

<div class="section_buttons">

| Previous                |                                                         Next |
|:------------------------|-------------------------------------------------------------:|
| [Building](building.md) | [Source Code](../third-party/doxyconfig/docs/source_code.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
