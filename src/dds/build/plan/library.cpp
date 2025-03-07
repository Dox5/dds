#include "./library.hpp"

#include <dds/util/algo.hpp>
#include <dds/util/log.hpp>

#include <range/v3/action/push_back.hpp>
#include <range/v3/range/conversion.hpp>
#include <range/v3/view/concat.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/transform.hpp>

#include <cassert>
#include <string>

using namespace dds;

namespace {

const std::string gen_dir_qual = "__dds/gen";

fs::path rebase_gen_incdir(path_ref subdir) { return gen_dir_qual / subdir; }
}  // namespace

std::optional<fs::path> library_plan::generated_include_dir() const noexcept {
    if (_templates.empty()) {
        return std::nullopt;
    }
    return rebase_gen_incdir(output_subdirectory());
}

library_plan library_plan::create(const library_root&             lib,
                                  const library_build_params&     params,
                                  std::optional<std::string_view> qual_name_) {
    const fs::path out_dir = params.out_subdir / lib.path_namespace();

    // Source files are kept in three groups:
    std::vector<source_file> app_sources;
    std::vector<source_file> test_sources;
    std::vector<source_file> lib_sources;
    std::vector<source_file> template_sources;
    std::vector<source_file> header_sources;
    std::vector<source_file> public_header_sources;

    auto qual_name = std::string(qual_name_.value_or(lib.manifest().name.str));

    // Collect the source for this library. This will look for any compilable sources in the
    // `src/` subdirectory of the library.
    auto src_dir = lib.src_source_root();
    if (src_dir.exists()) {
        // Sort each source file between the three source arrays, depending on
        // the kind of source that we are looking at.
        auto all_sources = src_dir.collect_sources();
        for (const auto& sfile : all_sources) {
            if (sfile.kind == source_kind::test) {
                test_sources.push_back(sfile);
            } else if (sfile.kind == source_kind::app) {
                app_sources.push_back(sfile);
            } else if (sfile.kind == source_kind::source) {
                lib_sources.push_back(sfile);
            } else if (sfile.kind == source_kind::header_template) {
                template_sources.push_back(sfile);
            } else if (sfile.kind == source_kind::header) {
                header_sources.push_back(sfile);
            } else {
                assert(sfile.kind == source_kind::header_impl);
            }
        }
    }

    auto include_dir = lib.include_source_root();
    if (include_dir.exists()) {
        auto all_sources = include_dir.collect_sources();
        for (const auto& sfile : all_sources) {
            if (!is_header(sfile.kind)) {
                dds_log(warn,
                        "Public include/ should only contain header or header template files."
                        " Not a header: {}",
                        sfile.path.string());
            } else if (sfile.kind == source_kind::header) {
                public_header_sources.push_back(sfile);
            }
        }
    }
    if (!params.build_tests) {
        public_header_sources.clear();
        header_sources.clear();
    }

    // Load up the compile rules
    shared_compile_file_rules compile_rules;
    lib.append_public_compile_rules(neo::into(compile_rules));
    compile_rules.enable_warnings() = params.enable_warnings;
    compile_rules.uses()            = lib.manifest().uses;

    const auto codegen_subdir = rebase_gen_incdir(out_dir);

    if (!template_sources.empty()) {
        compile_rules.include_dirs().push_back(codegen_subdir);
    }

    auto public_header_compile_rules          = compile_rules.clone();
    public_header_compile_rules.syntax_only() = true;
    auto src_header_compile_rules             = public_header_compile_rules.clone();
    lib.append_private_compile_rules(neo::into(compile_rules));
    lib.append_private_compile_rules(neo::into(src_header_compile_rules));

    // Convert the library sources into their respective file compilation plans.
    auto lib_compile_files =  //
        lib_sources           //
        | ranges::views::transform([&](const source_file& sf) {
              return compile_file_plan(compile_rules, sf, qual_name, out_dir / "obj");
          })
        | ranges::to_vector;

    // Run a syntax-only pass over headers to verify that headers can build in isolation.
    auto header_indep_plan = header_sources  //
        | ranges::views::transform([&](const source_file& sf) {
                                 return compile_file_plan(src_header_compile_rules,
                                                          sf,
                                                          qual_name,
                                                          out_dir / "timestamps");
                             })
        | ranges::to_vector;
    extend(header_indep_plan,
           public_header_sources | ranges::views::transform([&](const source_file& sf) {
               return compile_file_plan(public_header_compile_rules,
                                        sf,
                                        qual_name,
                                        out_dir / "timestamps");
           }));
    // If we have any compiled library files, generate a static library archive
    // for this library
    std::optional<create_archive_plan> archive_plan;
    if (!lib_compile_files.empty()) {
        dds_log(debug, "Generating an archive library for {}", qual_name);
        archive_plan.emplace(lib.manifest().name.str,
                             qual_name,
                             out_dir,
                             std::move(lib_compile_files));
    } else {
        dds_log(debug,
                "Library {} has no compiled inputs, so no archive will be generated",
                qual_name);
    }

    // Collect the paths to linker inputs that should be used when generating executables for this
    // library.
    std::vector<lm::usage> links;
    extend(links, lib.manifest().uses);
    extend(links, lib.manifest().links);

    // There may also be additional usage requirements for tests
    auto test_rules = compile_rules.clone();
    auto test_links = links;
    extend(test_rules.uses(), params.test_uses);
    extend(test_links, params.test_uses);

    // Generate the plans to link any executables for this library
    std::vector<link_executable_plan> link_executables;
    for (const source_file& source : ranges::views::concat(app_sources, test_sources)) {
        const bool is_test = source.kind == source_kind::test;
        if (is_test && !params.build_tests) {
            // This is a test, but we don't want to build tests
            continue;
        }
        if (!is_test && !params.build_apps) {
            // This is an app, but we don't want to build apps
            continue;
        }
        // Pick a subdir based on app/test
        const auto subdir_base = is_test ? out_dir / "test" : out_dir;
        // Put test/app executables in a further subdirectory based on the source file path
        const auto subdir = subdir_base / source.relative_path().parent_path();
        // Pick compile rules based on app/test
        auto rules = is_test ? test_rules : compile_rules;
        // Pick input libs based on app/test
        auto& exe_links = is_test ? test_links : links;
        // TODO: Apps/tests should only see the _public_ include dir, not both
        auto exe
            = link_executable_plan{exe_links,
                                   compile_file_plan(rules, source, qual_name, out_dir / "obj"),
                                   subdir,
                                   source.path.stem().stem().string()};
        link_executables.emplace_back(std::move(exe));
    }

    std::vector<render_template_plan> render_templates;
    for (const auto& sf : template_sources) {
        render_templates.emplace_back(sf, codegen_subdir);
    }

    // Done!
    return library_plan{lib,
                        qual_name,
                        out_dir,
                        std::move(archive_plan),
                        std::move(link_executables),
                        std::move(render_templates),
                        std::move(header_indep_plan)};
}
