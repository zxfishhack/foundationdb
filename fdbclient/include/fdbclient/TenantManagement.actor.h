/*
 * TenantManagement.actor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include "flow/IRandom.h"
#if defined(NO_INTELLISENSE) && !defined(FDBCLIENT_TENANT_MANAGEMENT_ACTOR_G_H)
#define FDBCLIENT_TENANT_MANAGEMENT_ACTOR_G_H
#include "fdbclient/TenantManagement.actor.g.h"
#elif !defined(FDBCLIENT_TENANT_MANAGEMENT_ACTOR_H)
#define FDBCLIENT_TENANT_MANAGEMENT_ACTOR_H

#include <string>
#include <map>
#include "fdbclient/GenericTransactionHelper.h"
#include "fdbclient/Metacluster.h"
#include "fdbclient/SystemData.h"
#include "flow/actorcompiler.h" // has to be last include

namespace TenantAPI {

template <class Transaction>
Future<Optional<TenantMapEntry>> tryGetTenantTransaction(Transaction tr, TenantName name) {
	tr->setOption(FDBTransactionOptions::RAW_ACCESS);
	return TenantMetadata::tenantMap.get(tr, name);
}

ACTOR template <class DB>
Future<Optional<TenantMapEntry>> tryGetTenant(Reference<DB> db, TenantName name) {
	state Reference<typename DB::TransactionT> tr = db->createTransaction();

	loop {
		try {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::READ_LOCK_AWARE);
			Optional<TenantMapEntry> entry = wait(tryGetTenantTransaction(tr, name));
			return entry;
		} catch (Error& e) {
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}

ACTOR template <class Transaction>
Future<TenantMapEntry> getTenantTransaction(Transaction tr, TenantName name) {
	Optional<TenantMapEntry> entry = wait(tryGetTenantTransaction(tr, name));
	if (!entry.present()) {
		throw tenant_not_found();
	}

	return entry.get();
}

ACTOR template <class DB>
Future<TenantMapEntry> getTenant(Reference<DB> db, TenantName name) {
	Optional<TenantMapEntry> entry = wait(tryGetTenant(db, name));
	if (!entry.present()) {
		throw tenant_not_found();
	}

	return entry.get();
}

ACTOR template <class Transaction>
Future<ClusterType> getClusterType(Transaction tr) {
	Optional<MetaclusterRegistrationEntry> metaclusterRegistration =
	    wait(MetaclusterMetadata::metaclusterRegistration.get(tr));

	return metaclusterRegistration.present() ? metaclusterRegistration.get().clusterType : ClusterType::STANDALONE;
}

ACTOR template <class Transaction>
Future<Void> checkTenantMode(Transaction tr, ClusterType expectedClusterType) {
	state typename transaction_future_type<Transaction, Optional<Value>>::type tenantModeFuture =
	    tr->get(configKeysPrefix.withSuffix("tenant_mode"_sr));

	state ClusterType actualClusterType = wait(getClusterType(tr));
	Optional<Value> tenantModeValue = wait(safeThreadFutureToFuture(tenantModeFuture));

	TenantMode tenantMode = TenantMode::fromValue(tenantModeValue.castTo<ValueRef>());
	if (actualClusterType != expectedClusterType) {
		throw invalid_metacluster_operation();
	} else if (actualClusterType == ClusterType::STANDALONE && tenantMode == TenantMode::DISABLED) {
		throw tenants_disabled();
	}

	return Void();
}

TenantMode tenantModeForClusterType(ClusterType clusterType, TenantMode tenantMode);

// Creates a tenant with the given name. If the tenant already exists, an empty optional will be returned.
ACTOR template <class Transaction>
Future<std::pair<Optional<TenantMapEntry>, bool>> createTenantTransaction(
    Transaction tr,
    TenantNameRef name,
    TenantMapEntry tenantEntry,
    ClusterType clusterType = ClusterType::STANDALONE) {

	state bool allowSubspace = clusterType == ClusterType::STANDALONE;

	ASSERT(clusterType != ClusterType::METACLUSTER_MANAGEMENT);
	ASSERT(tenantEntry.id >= 0);

	ASSERT(tenantEntry.id >= 0);

	if (name.startsWith("\xff"_sr)) {
		throw invalid_tenant_name();
	}

	tr->setOption(FDBTransactionOptions::RAW_ACCESS);

	state Future<Optional<TenantMapEntry>> existingEntryFuture = tryGetTenantTransaction(tr, name);
	state typename transaction_future_type<Transaction, Optional<Value>>::type tenantDataPrefixFuture;
	if (allowSubspace) {
		tenantDataPrefixFuture = tr->get(tenantDataPrefixKey);
	}

	state Future<Void> tenantModeCheck = checkTenantMode(tr, clusterType);
	state Future<bool> tombstoneFuture = TenantMetadata::tenantTombstones.exists(tr, tenantEntry.id);

	wait(tenantModeCheck);
	Optional<TenantMapEntry> existingEntry = wait(existingEntryFuture);
	if (existingEntry.present()) {
		return std::make_pair(existingEntry.get(), false);
	}

	state bool hasTombstone = wait(tombstoneFuture);
	if (hasTombstone) {
		return std::make_pair(Optional<TenantMapEntry>(), false);
	}

	if (allowSubspace) {
		Optional<Value> tenantDataPrefix = wait(safeThreadFutureToFuture(tenantDataPrefixFuture));
		if (tenantDataPrefix.present() &&
		    tenantDataPrefix.get().size() + TenantMapEntry::ROOT_PREFIX_SIZE > CLIENT_KNOBS->TENANT_PREFIX_SIZE_LIMIT) {
			TraceEvent(SevWarnAlways, "TenantPrefixTooLarge")
			    .detail("TenantSubspace", tenantDataPrefix.get())
			    .detail("TenantSubspaceLength", tenantDataPrefix.get().size())
			    .detail("RootPrefixLength", TenantMapEntry::ROOT_PREFIX_SIZE)
			    .detail("MaxTenantPrefixSize", CLIENT_KNOBS->TENANT_PREFIX_SIZE_LIMIT);

			throw client_invalid_operation();
		}
		tenantEntry.setSubspace(tenantDataPrefix.present() ? (KeyRef)tenantDataPrefix.get() : ""_sr);
	} else {
		tenantEntry.setSubspace(""_sr);
	}

	state typename transaction_future_type<Transaction, RangeResult>::type prefixRangeFuture =
	    tr->getRange(prefixRange(tenantEntry.prefix), 1);

	RangeResult contents = wait(safeThreadFutureToFuture(prefixRangeFuture));
	if (!contents.empty()) {
		throw tenant_prefix_allocator_conflict();
	}

	tenantEntry.tenantState = TenantState::READY;
	tenantEntry.assignedCluster = Optional<ClusterName>();

	TenantMetadata::tenantMap.set(tr, name, tenantEntry);
	if (tenantEntry.tenantGroup.present()) {
		TenantMetadata::tenantGroupTenantIndex.insert(tr, Tuple::makeTuple(tenantEntry.tenantGroup.get(), name));
	}

	return std::make_pair(tenantEntry, true);
}

ACTOR template <class Transaction>
Future<int64_t> getNextTenantId(Transaction tr) {
	Optional<int64_t> lastId = wait(TenantMetadata::lastTenantId.get(tr));
	int64_t tenantId = lastId.orDefault(-1) + 1;
	if (BUGGIFY) {
		tenantId += deterministicRandom()->randomSkewedUInt32(1, 1e9);
	}
	return tenantId;
}

ACTOR template <class DB>
Future<Optional<TenantMapEntry>> createTenant(Reference<DB> db,
                                              TenantName name,
                                              TenantMapEntry tenantEntry = TenantMapEntry(),
                                              ClusterType clusterType = ClusterType::STANDALONE) {
	state Reference<typename DB::TransactionT> tr = db->createTransaction();

	state bool checkExistence = clusterType != ClusterType::METACLUSTER_DATA;
	state bool generateTenantId = tenantEntry.id < 0;

	ASSERT(clusterType == ClusterType::STANDALONE || !generateTenantId);

	loop {
		try {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);

			state Future<int64_t> tenantIdFuture;
			if (generateTenantId) {
				tenantIdFuture = getNextTenantId(tr);
			}

			if (checkExistence) {
				Optional<TenantMapEntry> entry = wait(tryGetTenantTransaction(tr, name));
				if (entry.present()) {
					throw tenant_already_exists();
				}

				checkExistence = false;
			}

			if (generateTenantId) {
				int64_t tenantId = wait(tenantIdFuture);
				tenantEntry.id = tenantId;
				TenantMetadata::lastTenantId.set(tr, tenantEntry.id);
			}

			state std::pair<Optional<TenantMapEntry>, bool> newTenant =
			    wait(createTenantTransaction(tr, name, tenantEntry, clusterType));

			if (newTenant.second) {
				ASSERT(newTenant.first.present());
				wait(buggifiedCommit(tr, BUGGIFY_WITH_PROB(0.1)));

				TraceEvent("CreatedTenant")
				    .detail("Tenant", name)
				    .detail("TenantId", newTenant.first.get().id)
				    .detail("Prefix", newTenant.first.get().prefix)
				    .detail("TenantGroup", tenantEntry.tenantGroup)
				    .detail("Version", tr->getCommittedVersion());
			}

			return newTenant.first;
		} catch (Error& e) {
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}

ACTOR template <class Transaction>
Future<Void> deleteTenantTransaction(Transaction tr,
                                     TenantNameRef name,
                                     Optional<int64_t> tenantId = Optional<int64_t>(),
                                     ClusterType clusterType = ClusterType::STANDALONE) {
	ASSERT(clusterType == ClusterType::STANDALONE || tenantId.present());
	ASSERT(clusterType != ClusterType::METACLUSTER_MANAGEMENT);

	tr->setOption(FDBTransactionOptions::RAW_ACCESS);

	state Future<Optional<TenantMapEntry>> tenantEntryFuture = tryGetTenantTransaction(tr, name);
	wait(checkTenantMode(tr, clusterType));

	state Optional<TenantMapEntry> tenantEntry = wait(tenantEntryFuture);
	if (tenantEntry.present() && (!tenantId.present() || tenantEntry.get().id == tenantId.get())) {
		state typename transaction_future_type<Transaction, RangeResult>::type prefixRangeFuture =
		    tr->getRange(prefixRange(tenantEntry.get().prefix), 1);

		RangeResult contents = wait(safeThreadFutureToFuture(prefixRangeFuture));
		if (!contents.empty()) {
			throw tenant_not_empty();
		}

		TenantMetadata::tenantMap.erase(tr, name);
		if (tenantEntry.get().tenantGroup.present()) {
			TenantMetadata::tenantGroupTenantIndex.erase(tr,
			                                             Tuple::makeTuple(tenantEntry.get().tenantGroup.get(), name));
		}

		if (clusterType == ClusterType::METACLUSTER_DATA) {
			// In data clusters, we store a tombstone
			// TODO: periodically clean up tombstones
			TenantMetadata::tenantTombstones.insert(tr, tenantId.get());
		}
	}

	return Void();
}

ACTOR template <class DB>
Future<Void> deleteTenant(Reference<DB> db,
                          TenantName name,
                          Optional<int64_t> tenantId = Optional<int64_t>(),
                          ClusterType clusterType = ClusterType::STANDALONE) {
	state Reference<typename DB::TransactionT> tr = db->createTransaction();

	state bool checkExistence = clusterType == ClusterType::STANDALONE;
	loop {
		try {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);

			if (checkExistence) {
				Optional<TenantMapEntry> entry = wait(tryGetTenantTransaction(tr, name));
				if (!entry.present()) {
					throw tenant_not_found();
				}

				checkExistence = false;
			}

			wait(deleteTenantTransaction(tr, name, tenantId, clusterType));
			wait(buggifiedCommit(tr, BUGGIFY_WITH_PROB(0.1)));

			TraceEvent("DeletedTenant").detail("Tenant", name).detail("Version", tr->getCommittedVersion());
			return Void();
		} catch (Error& e) {
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}

// This should only be called from a transaction that has already confirmed that the tenant entry
// is present. The tenantEntry should start with the existing entry and modify only those fields that need
// to be changed. This must only be called on a non-management cluster.
template <class Transaction>
void configureTenantTransaction(Transaction tr, TenantNameRef tenantName, TenantMapEntry tenantEntry) {
	tr->setOption(FDBTransactionOptions::RAW_ACCESS);
	TenantMetadata::tenantMap.set(tr, tenantName, tenantEntry);
}

ACTOR template <class Transaction>
Future<std::vector<std::pair<TenantName, TenantMapEntry>>> listTenantsTransaction(Transaction tr,
                                                                                  TenantNameRef begin,
                                                                                  TenantNameRef end,
                                                                                  int limit) {
	tr->setOption(FDBTransactionOptions::RAW_ACCESS);

	KeyBackedRangeResult<std::pair<TenantName, TenantMapEntry>> results =
	    wait(TenantMetadata::tenantMap.getRange(tr, begin, end, limit));

	return results.results;
}

ACTOR template <class DB>
Future<std::vector<std::pair<TenantName, TenantMapEntry>>> listTenants(Reference<DB> db,
                                                                       TenantName begin,
                                                                       TenantName end,
                                                                       int limit) {
	state Reference<typename DB::TransactionT> tr = db->createTransaction();

	loop {
		try {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::READ_LOCK_AWARE);
			std::vector<std::pair<TenantName, TenantMapEntry>> tenants =
			    wait(listTenantsTransaction(tr, begin, end, limit));
			return tenants;
		} catch (Error& e) {
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}

ACTOR template <class DB>
Future<Void> renameTenant(Reference<DB> db, TenantName oldName, TenantName newName) {
	state Reference<typename DB::TransactionT> tr = db->createTransaction();

	state bool firstTry = true;
	state int64_t id;
	loop {
		try {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			state Optional<TenantMapEntry> oldEntry;
			state Optional<TenantMapEntry> newEntry;
			wait(store(oldEntry, tryGetTenantTransaction(tr, oldName)) &&
			     store(newEntry, tryGetTenantTransaction(tr, newName)));
			if (firstTry) {
				if (!oldEntry.present()) {
					throw tenant_not_found();
				}
				if (newEntry.present()) {
					throw tenant_already_exists();
				}
				// Store the id we see when first reading this key
				id = oldEntry.get().id;

				firstTry = false;
			} else {
				// If we got commit_unknown_result, the rename may have already occurred.
				if (newEntry.present()) {
					int64_t checkId = newEntry.get().id;
					if (id == checkId) {
						ASSERT(!oldEntry.present() || oldEntry.get().id != id);
						return Void();
					}
					// If the new entry is present but does not match, then
					// the rename should fail, so we throw an error.
					throw tenant_already_exists();
				}
				if (!oldEntry.present()) {
					throw tenant_not_found();
				}
				int64_t checkId = oldEntry.get().id;
				// If the id has changed since we made our first attempt,
				// then it's possible we've already moved the tenant. Don't move it again.
				if (id != checkId) {
					throw tenant_not_found();
				}
			}

			TenantMetadata::tenantMap.erase(tr, oldName);
			TenantMetadata::tenantMap.set(tr, newName, oldEntry.get());

			wait(safeThreadFutureToFuture(tr->commit()));
			TraceEvent("RenameTenantSuccess").detail("OldName", oldName).detail("NewName", newName);
			return Void();
		} catch (Error& e) {
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}
} // namespace TenantAPI

#include "flow/unactorcompiler.h"
#endif
