// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/intrusive_c_tailq.hpp>
#include <reloco/intrusive_splay_tree.hpp>
#include <reloco/lifetime.hpp>
#include <structo/backref_owner.hpp>

#include <functional>

namespace {

struct owner {
  int id = 0;
};

struct holder {
  structo::backref_ptr<owner> parent;
  int key = 0;

  struct {
    holder *next = nullptr;
    holder **prev = nullptr;
  } memq_link;

  reloco::intrusive_splay_tree_hook<holder> tree_link;
};

// `Container` adapter #1: FreeBSD vm_object::memq-style walk-everything tail queue.
template <typename Holder, auto Hook> struct tailq_container {
  reloco::c_tailq<Holder, Hook> list;

  reloco::result<void> insert(Holder &h) & noexcept {
    list.push_back(h);
    return {};
  }
  void remove(Holder &h) & noexcept { list.remove(h); }
  template <typename F> void clear_and_dispose(F &&fn) & noexcept {
    while (Holder *h = list.pop_front())
      fn(*h);
  }
  template <typename F> void for_each(F &&fn) & noexcept {
    for (Holder &h : list)
      fn(h);
  }
  template <typename Pred, typename Disposer> void remove_if(Pred &&pred, Disposer &&disposer) & noexcept {
    for (auto it = list.begin(); it != list.end();) {
      Holder &h = *it;
      ++it; // advance past `h` before possibly unlinking it
      if (pred(h)) {
        list.remove(h);
        disposer(h);
      }
    }
  }
  [[nodiscard]] bool empty() const & noexcept { return list.empty(); }
};

// `Container` adapter #2: keyed-by-offset splay tree, e.g. a page cache looking pages up by key.
struct holder_key_of {
  int operator()(const holder &h) const noexcept { return h.key; }
};

template <typename Holder, reloco::intrusive_splay_tree_hook<Holder> Holder::*Hook, typename KeyOf>
struct splay_container {
  reloco::intrusive_splay_tree<Holder, Hook, KeyOf> tree;

  reloco::result<void> insert(Holder &h) & noexcept { return tree.try_insert(h); }
  void remove(Holder &h) & noexcept { tree.remove(h); }
  template <typename F> void clear_and_dispose(F &&fn) & noexcept { tree.clear_and_dispose(std::forward<F>(fn)); }
  template <typename F> void for_each(F &&fn) & noexcept {
    for (Holder &h : tree)
      fn(h);
  }
  template <typename Pred, typename Disposer> void remove_if(Pred &&pred, Disposer &&disposer) & noexcept {
    for (auto it = tree.begin(); it != tree.end();) {
      if (pred(*it)) {
        Holder &h = *it;
        it = tree.erase(it);
        disposer(h);
      } else {
        ++it;
      }
    }
  }
  [[nodiscard]] bool empty() const & noexcept { return tree.empty(); }

  template <typename K> reloco::result<std::reference_wrapper<Holder>> try_find(const K &key) & noexcept {
    return tree.try_find(key);
  }
};

using tailq_registry = structo::backref_owner<&holder::parent, tailq_container<holder, &holder::memq_link>>;
using splay_registry =
    structo::backref_owner<&holder::parent, splay_container<holder, &holder::tree_link, holder_key_of>>;

TEST(BackrefOwnerTest, TailqAttachLinksAndSetsBackref) {
  owner o{1};
  holder h;
  tailq_registry reg;

  auto r = reg.attach(o, h);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(h.parent.lock().get(), &o);
  EXPECT_FALSE(reg.empty());

  reg.detach(h);
  EXPECT_EQ(h.parent.lock().get(), nullptr);
  EXPECT_TRUE(reg.empty());
}

TEST(BackrefOwnerTest, TailqTryAttachAndTryDetachRoundTrip) {
  owner o{1};
  holder h;
  tailq_registry reg;

  ASSERT_TRUE(reg.try_attach(o, h).has_value());
  EXPECT_EQ(h.parent.lock().get(), &o);

  ASSERT_TRUE(reg.try_detach(h).has_value());
  EXPECT_EQ(h.parent.lock().get(), nullptr);
}

TEST(BackrefOwnerTest, TailqDetachAllClearsEveryHolder) {
  owner o{1};
  holder a, b, c;
  tailq_registry reg;

  ASSERT_TRUE(reg.attach(o, a).has_value());
  ASSERT_TRUE(reg.attach(o, b).has_value());
  ASSERT_TRUE(reg.attach(o, c).has_value());
  EXPECT_FALSE(reg.empty());

  reg.detach_all();

  EXPECT_EQ(a.parent.lock().get(), nullptr);
  EXPECT_EQ(b.parent.lock().get(), nullptr);
  EXPECT_EQ(c.parent.lock().get(), nullptr);
  EXPECT_TRUE(reg.empty());
}

TEST(BackrefOwnerDeathTest, DestructionWithAttachedHolderTraps) {
  owner o{1};
  holder h;
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH(
      {
        tailq_registry reg;
        auto r = reg.attach(o, h);
        (void)r;
      },
      "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
  // The dying registry still linked `h`; detach it manually so it is not
  // left pointing at a since-destroyed registry for any later test.
  h.parent.lock().reset(nullptr);
}

TEST(BackrefOwnerTest, SplayAttachLinksAndSetsBackref) {
  owner o{1};
  holder h;
  h.key = 5;
  splay_registry reg;

  auto r = reg.attach(o, h);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(h.parent.lock().get(), &o);

  reg.detach(h);
  EXPECT_EQ(h.parent.lock().get(), nullptr);
}

TEST(BackrefOwnerTest, SplayAttachFailsOnDuplicateKeyAndLeavesBackrefUntouched) {
  owner o{1};
  holder a, b;
  a.key = 9;
  b.key = 9;
  splay_registry reg;

  ASSERT_TRUE(reg.attach(o, a).has_value());
  auto r = reg.attach(o, b);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::already_exists);
  EXPECT_EQ(b.parent.lock().get(), nullptr);

  reg.detach(a);
}

TEST(BackrefOwnerTest, SplayDetachAllClearsEveryHolder) {
  owner o{1};
  holder a, b, c;
  a.key = 1;
  b.key = 2;
  c.key = 3;
  splay_registry reg;

  ASSERT_TRUE(reg.attach(o, a).has_value());
  ASSERT_TRUE(reg.attach(o, b).has_value());
  ASSERT_TRUE(reg.attach(o, c).has_value());

  reg.detach_all();

  EXPECT_EQ(a.parent.lock().get(), nullptr);
  EXPECT_EQ(b.parent.lock().get(), nullptr);
  EXPECT_EQ(c.parent.lock().get(), nullptr);
  EXPECT_TRUE(reg.empty());
}

TEST(BackrefOwnerTest, TailqMigrateToMovesHolderAndReassignsBackref) {
  owner shadow{1};
  owner backing{2};
  holder h;
  tailq_registry shadow_pages;
  tailq_registry backing_pages;

  ASSERT_TRUE(shadow_pages.attach(shadow, h).has_value());

  auto r = shadow_pages.migrate_to(backing_pages, backing, h);
  ASSERT_TRUE(r.has_value());

  EXPECT_TRUE(shadow_pages.empty());
  EXPECT_FALSE(backing_pages.empty());
  EXPECT_EQ(h.parent.lock().get(), &backing);

  backing_pages.detach(h);
}

TEST(BackrefOwnerTest, TailqTryMigrateToMovesHolderAndReassignsBackref) {
  owner shadow{1};
  owner backing{2};
  holder h;
  tailq_registry shadow_pages;
  tailq_registry backing_pages;

  ASSERT_TRUE(shadow_pages.attach(shadow, h).has_value());

  auto r = shadow_pages.try_migrate_to(backing_pages, backing, h);
  ASSERT_TRUE(r.has_value());

  EXPECT_TRUE(shadow_pages.empty());
  EXPECT_EQ(h.parent.lock().get(), &backing);

  backing_pages.detach(h);
}

TEST(BackrefOwnerTest, SplayMigrateToFailsOnDuplicateKeyAndRollsBack) {
  owner shadow{1};
  owner backing{2};
  holder already_there;
  already_there.key = 3;
  holder migrating;
  migrating.key = 3; // same key -- the backing registry already has one.

  splay_registry shadow_pages;
  splay_registry backing_pages;

  ASSERT_TRUE(shadow_pages.attach(shadow, migrating).has_value());
  ASSERT_TRUE(backing_pages.attach(backing, already_there).has_value());

  auto r = shadow_pages.migrate_to(backing_pages, backing, migrating);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::already_exists);

  // Rolled back: still attached to `shadow`, not `backing`.
  EXPECT_EQ(migrating.parent.lock().get(), &shadow);
  EXPECT_FALSE(shadow_pages.empty());

  shadow_pages.detach(migrating);
  backing_pages.detach(already_there);
}

TEST(BackrefOwnerTest, SplayMigrateToMovesHolderAndReassignsBackref) {
  owner shadow{1};
  owner backing{2};
  holder h;
  h.key = 7;
  splay_registry shadow_pages;
  splay_registry backing_pages;

  ASSERT_TRUE(shadow_pages.attach(shadow, h).has_value());

  auto r = shadow_pages.migrate_to(backing_pages, backing, h);
  ASSERT_TRUE(r.has_value());

  EXPECT_TRUE(shadow_pages.empty());
  EXPECT_EQ(h.parent.lock().get(), &backing);

  backing_pages.detach(h);
}

TEST(BackrefOwnerTest, TailqForEachVisitsEveryHolderWithoutDetaching) {
  owner o{1};
  holder a, b, c;
  a.key = 1;
  b.key = 2;
  c.key = 3;
  tailq_registry reg;

  ASSERT_TRUE(reg.attach(o, a).has_value());
  ASSERT_TRUE(reg.attach(o, b).has_value());
  ASSERT_TRUE(reg.attach(o, c).has_value());

  int sum = 0;
  reg.for_each([&](holder &h) noexcept { sum += h.key; });
  EXPECT_EQ(sum, 6);

  // Nothing was unlinked or detached.
  EXPECT_FALSE(reg.empty());
  EXPECT_EQ(a.parent.lock().get(), &o);
  EXPECT_EQ(b.parent.lock().get(), &o);
  EXPECT_EQ(c.parent.lock().get(), &o);

  reg.detach_all();
}

TEST(BackrefOwnerTest, TailqDetachIfDetachesOnlyMatchingHolders) {
  owner o{1};
  holder a, b, c;
  a.key = 1;
  b.key = 2;
  c.key = 3;
  tailq_registry reg;

  ASSERT_TRUE(reg.attach(o, a).has_value());
  ASSERT_TRUE(reg.attach(o, b).has_value());
  ASSERT_TRUE(reg.attach(o, c).has_value());

  reg.detach_if([](holder &h) noexcept { return h.key >= 2; });

  // `a` (key 1) is still attached; `b` and `c` (key >= 2) were detached.
  EXPECT_EQ(a.parent.lock().get(), &o);
  EXPECT_EQ(b.parent.lock().get(), nullptr);
  EXPECT_EQ(c.parent.lock().get(), nullptr);
  EXPECT_FALSE(reg.empty());

  reg.detach(a);
}

TEST(BackrefOwnerTest, SplayDetachIfDetachesOnlyMatchingHolders) {
  owner o{1};
  holder a, b, c;
  a.key = 1;
  b.key = 2;
  c.key = 3;
  splay_registry reg;

  ASSERT_TRUE(reg.attach(o, a).has_value());
  ASSERT_TRUE(reg.attach(o, b).has_value());
  ASSERT_TRUE(reg.attach(o, c).has_value());

  reg.detach_if([](holder &h) noexcept { return h.key == 2; });

  EXPECT_EQ(a.parent.lock().get(), &o);
  EXPECT_EQ(b.parent.lock().get(), nullptr);
  EXPECT_EQ(c.parent.lock().get(), &o);

  reg.detach(a);
  reg.detach(c);
}

TEST(BackrefOwnerTest, SplayTryFindLocatesAttachedHolderByKey) {
  owner o{1};
  holder a, b;
  a.key = 4;
  b.key = 8;
  splay_registry reg;

  ASSERT_TRUE(reg.attach(o, a).has_value());
  ASSERT_TRUE(reg.attach(o, b).has_value());

  auto found = reg.try_find(8);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(&found.value().get(), &b);

  auto missing = reg.try_find(999);
  EXPECT_FALSE(missing.has_value());

  reg.detach(a);
  reg.detach(b);
}

} // namespace
