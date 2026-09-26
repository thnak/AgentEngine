// Proof for ADR-037: agentengine::rt::ProjectRegistry (include/agentengine/rt/project_registry.hpp),
// the Quark-actor-free replacement for agentengine::ProjectRegistry's in-memory index. Mirrors the
// original's own test_project_registry.cpp H1-H3 shape (H4/H5/G4 -- project/project.hpp's manifest
// snapshot and archived-member tail -- are a separate, larger gap named in this file's own header
// comment, not covered here).
//   H1 -- create_project registers the id against its principal; a duplicate id fails closed.
//   H2 -- list_projects is scoped on BOTH principal_id and principal_tenant_id (018 Sec6's
//         multi-tenant shape: the same principal id under two tenants must not see each other).
//   H3 -- restore_index() repopulates a FRESH registry's index so it answers identically to the
//         live one, including correctly rejecting a duplicate of an id it only ever learned about
//         via restore, never via its own create_project.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/rt/project_registry.hpp"

using agentengine::rt::CreateProject;
using agentengine::rt::CreateProjectResult;
using agentengine::rt::ListProjects;
using agentengine::rt::ListProjectsResult;
using agentengine::rt::ProjectRegistry;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// Safe here: create_project()/list_projects()'s only suspension point is mutex_'s uncontended fast
// path in this single-caller test file -- one resume() call resolves each fully.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    // ---- H1: create_project registers the id; a duplicate id fails closed ------------------------
    ProjectRegistry registry;
    std::string const leo_id = "p-leo", leo_tenant = "tenant-a";

    auto r1 = drive(registry.create_project(CreateProject{"proj-1", leo_id, leo_tenant}));
    check(r1.ok, "H1: create_project succeeds");

    auto r_dup = drive(registry.create_project(CreateProject{"proj-1", leo_id, leo_tenant}));
    check(!r_dup.ok && r_dup.error_code == "project.duplicate_id",
          "H1 (fails closed): a second create_project with the SAME id is rejected, not silently "
          "overwriting the original -- a retried create must not stomp it");

    // ---- H2: list_projects is scoped on BOTH principal_id and principal_tenant_id -----------------
    std::string const mia_id = "p-mia", mia_tenant = "tenant-a";
    std::string const leo_tenant_b = "tenant-b";  // same id as leo, DIFFERENT tenant
    (void)drive(registry.create_project(CreateProject{"proj-2", leo_id, leo_tenant}));
    (void)drive(registry.create_project(CreateProject{"proj-3", mia_id, mia_tenant}));
    (void)drive(registry.create_project(CreateProject{"proj-4", leo_id, leo_tenant_b}));

    auto leo_list = drive(registry.list_projects(ListProjects{leo_id, leo_tenant}));
    check(leo_list.project_ids.size() == 2, "H2: leo/tenant-a owns 2 Projects");
    if (leo_list.project_ids.size() == 2) {
        bool has1 = false, has2 = false;
        for (auto const& id : leo_list.project_ids) {
            if (id == "proj-1") has1 = true;
            if (id == "proj-2") has2 = true;
        }
        check(has1 && has2, "H2: exactly proj-1 and proj-2 are listed for leo/tenant-a");
    }
    auto mia_list = drive(registry.list_projects(ListProjects{mia_id, mia_tenant}));
    check(mia_list.project_ids.size() == 1 && mia_list.project_ids[0] == "proj-3",
          "H2: mia/tenant-a owns exactly proj-3");
    auto leo_b_list = drive(registry.list_projects(ListProjects{leo_id, leo_tenant_b}));
    check(leo_b_list.project_ids.size() == 1 && leo_b_list.project_ids[0] == "proj-4",
          "H2 (030 Sec7 G3): 'p-leo' under tenant-b sees ONLY proj-4, not proj-1/proj-2 from "
          "tenant-a -- same principal id, different tenant, no cross-tenant leak");
    auto stranger_list = drive(registry.list_projects(ListProjects{"p-nobody", ""}));
    check(stranger_list.project_ids.empty(), "H2: an unrelated principal sees no Projects at all");

    // ---- H3: restore_index() repopulates a FRESH registry to answer identically to the live one --
    {
        ProjectRegistry fresh;
        fresh.restore_index(registry.entries());

        auto fresh_leo_list = drive(fresh.list_projects(ListProjects{leo_id, leo_tenant}));
        check(fresh_leo_list.project_ids.size() == 2,
              "H3: a registry that never itself ran create_project, restored ONLY from entries(), "
              "answers list_projects identically to the live one");

        auto fresh_dup = drive(fresh.create_project(CreateProject{"proj-1", leo_id, leo_tenant}));
        check(!fresh_dup.ok,
              "H3: the restored registry also correctly rejects a duplicate of an id it only ever "
              "learned about via restore, never via its own create_project");
    }

    if (g_failures == 0) {
        std::printf("test_rt_project_registry: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_project_registry: %d failure(s)\n", g_failures);
    return 1;
}
