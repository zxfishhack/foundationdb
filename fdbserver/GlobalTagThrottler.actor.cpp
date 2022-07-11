/*
 * GlobalTagThrottler.actor.cpp
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

#include "fdbclient/FDBTypes.h"
#include "fdbclient/TagThrottle.actor.h"
#include "fdbrpc/Smoother.h"
#include "fdbserver/TagThrottler.h"

#include <limits>

#include "flow/actorcompiler.h" // must be last include

// In the function names below, several terms are used repeatedly. The context-specific are defined here:
//
// Cost: Every read or write operation has an associated cost, determined by the number of bytes accessed.
//       Global tag throttling quotas are specified in terms of the amount of this cost that can be consumed
//       per second. In the global tag throttler, cost refers to the per second rate of cost consumption.
//
// TPS: Transactions per second. Quotas are not specified in terms of TPS, but the limits given to clients must
//      be specified in terms of TPS because throttling is performed at the front end of transactions (before costs are
//      known).
//
// Total: Refers to the total quota specified by clients through the global tag throttling API. The sum of the
//        costs of all operations (cluster-wide) with a particular tag cannot exceed the tag's specified total quota,
//        even if the cluster has no saturated processes.
//
// Desired TPS: Assuming that a tag is able to achieve its total quota, this is the TPS it would be able to perform.
//
// Reserved: Refers to the reserved quota specified by clients through the global tag throttling API. As long as the
//           sum of the costs of all operations (cluster-wide) with a particular tag are not above the tag's
//           specified reserved quota, the tag should not experience any throttling from the global tag throttler.
//
// Current [Cost|TPS]: Measuring the current throughput on the cluster, independent of any specified quotas.
//
// ThrottlingRatio: Based on the health of each storage server, a throttling ratio is provided,
//                  informing the global tag throttler what ratio of the current throughput can be maintained.
//
// Limiting [Cost|TPS]: Based on the health of storage servers, a limiting throughput may be enforced.
//
// Target [Cost|TPS]: Based on reserved, limiting, and desired throughputs, this is the target throughput
//                    that the global tag throttler aims to achieve (across all clients).
//
// PerClient TPS: Because the target throughput must be shared across multiple clients, and all clients must
//           be given the same limits, a per-client limit is calculated based on the current and target throughputs.

class GlobalTagThrottlerImpl {

	enum class LimitType { RESERVED, TOTAL };
	enum class OpType { READ, WRITE };

	template <class K, class V>
	static Optional<V> get(std::unordered_map<K, V> const& m, K const& k) {
		auto it = m.find(k);
		if (it == m.end()) {
			return {};
		} else {
			return it->second;
		}
	}

	class ThroughputCounters {
		Smoother readCost;
		Smoother writeCost;

	public:
		ThroughputCounters()
		  : readCost(SERVER_KNOBS->GLOBAL_TAG_THROTTLING_FOLDING_TIME),
		    writeCost(SERVER_KNOBS->GLOBAL_TAG_THROTTLING_FOLDING_TIME) {}

		// Returns difference between new and current rates
		double updateCost(double newCost, OpType opType) {
			if (opType == OpType::READ) {
				auto const currentReadCost = readCost.getTotal();
				readCost.setTotal(newCost);
				return newCost - currentReadCost;
			} else {
				auto const currentWriteCost = writeCost.getTotal();
				writeCost.setTotal(newCost);
				return newCost - currentWriteCost;
			}
		}

		double getCost(OpType opType) const {
			if (opType == OpType::READ) {
				return readCost.smoothTotal();
			} else {
				return writeCost.smoothTotal();
			}
		}
	};

	// Track various statistics per tag, aggregated across all storage servers
	class PerTagStatistics {
		Optional<ThrottleApi::TagQuotaValue> quota;
		Smoother transactionCounter;
		Smoother perClientRate;

	public:
		explicit PerTagStatistics()
		  : transactionCounter(SERVER_KNOBS->GLOBAL_TAG_THROTTLING_FOLDING_TIME),
		    perClientRate(SERVER_KNOBS->GLOBAL_TAG_THROTTLING_FOLDING_TIME) {}

		Optional<ThrottleApi::TagQuotaValue> getQuota() const { return quota; }

		void setQuota(ThrottleApi::TagQuotaValue quota) { this->quota = quota; }

		void clearQuota() { quota = {}; }

		void addTransactions(int count) { transactionCounter.addDelta(count); }

		double getTransactionRate() const { return transactionCounter.smoothRate(); }

		Optional<ClientTagThrottleLimits> updateAndGetPerClientLimit(Optional<double> targetCost) {
			if (targetCost.present() && transactionCounter.smoothRate() > 0) {
				auto newPerClientRate = std::max(
				    SERVER_KNOBS->GLOBAL_TAG_THROTTLING_MIN_RATE,
				    std::min(targetCost.get(),
				             (targetCost.get() / transactionCounter.smoothRate()) * perClientRate.smoothTotal()));
				perClientRate.setTotal(newPerClientRate);
				return ClientTagThrottleLimits(perClientRate.getTotal(), ClientTagThrottleLimits::NO_EXPIRATION);
			} else {
				return {};
			}
		}
	};

	Database db;
	UID id;
	uint64_t throttledTagChangeId{ 0 };

	std::unordered_map<UID, Optional<double>> throttlingRatios;
	std::unordered_map<TransactionTag, PerTagStatistics> tagStatistics;
	std::unordered_map<UID, std::unordered_map<TransactionTag, ThroughputCounters>> throughput;

	// Returns the cost rate for the given tag on the given storage server
	Optional<double> getCurrentCost(UID storageServerId, TransactionTag tag, OpType opType) const {
		auto const tagToThroughputCounters = get(throughput, storageServerId);
		if (!tagToThroughputCounters.present()) {
			return {};
		}
		auto const throughputCounter = get(tagToThroughputCounters.get(), tag);
		if (!throughputCounter.present()) {
			return {};
		}
		return throughputCounter.get().getCost(opType);
	}

	// Return the cost rate on the given storage server, summed across all tags
	Optional<double> getCurrentCost(UID storageServerId, OpType opType) const {
		auto tagToPerTagThroughput = get(throughput, storageServerId);
		if (!tagToPerTagThroughput.present()) {
			return {};
		}
		double result = 0;
		for (const auto& [tag, perTagThroughput] : tagToPerTagThroughput.get()) {
			result += perTagThroughput.getCost(opType);
		}
		return result;
	}

	// Return the cost rate for the given tag, summed across all storage servers
	double getCurrentCost(TransactionTag tag, OpType opType) const {
		double result{ 0.0 };
		for (const auto& [id, _] : throughput) {
			result += getCurrentCost(id, tag, opType).orDefault(0);
		}
		return result;
	}

	// For transactions with the provided tag, returns the average cost that gets associated with the provided storage
	// server
	Optional<double> getAverageTransactionCost(TransactionTag tag, UID storageServerId, OpType opType) const {
		auto const cost = getCurrentCost(storageServerId, tag, opType);
		if (!cost.present()) {
			return {};
		}
		auto const stats = get(tagStatistics, tag);
		if (!stats.present()) {
			return {};
		}
		auto const transactionRate = stats.get().getTransactionRate();
		if (transactionRate == 0.0) {
			return {};
		} else {
			return cost.get() / transactionRate;
		}
	}

	// For transactions with the provided tag, returns the average cost
	Optional<double> getAverageTransactionCost(TransactionTag tag, OpType opType) const {
		auto const cost = getCurrentCost(tag, opType);
		auto const stats = get(tagStatistics, tag);
		if (!stats.present()) {
			return {};
		}
		auto const transactionRate = stats.get().getTransactionRate();
		if (transactionRate == 0.0) {
			return {};
		} else {
			return cost / transactionRate;
		}
	}

	// Returns the list of all tags performing meaningful work on the given storage server
	std::vector<TransactionTag> getTagsAffectingStorageServer(UID storageServerId) const {
		std::vector<TransactionTag> result;
		auto const tagToThroughputCounters = get(throughput, storageServerId);
		if (!tagToThroughputCounters.present()) {
			return {};
		} else {
			result.reserve(tagToThroughputCounters.get().size());
			for (const auto& [t, _] : tagToThroughputCounters.get()) {
				result.push_back(t);
			}
		}
		return result;
	}

	Optional<double> getQuota(TransactionTag tag, OpType opType, LimitType limitType) const {
		auto const stats = get(tagStatistics, tag);
		if (!stats.present()) {
			return {};
		}
		auto const quota = stats.get().getQuota();
		if (!quota.present()) {
			return {};
		}
		if (limitType == LimitType::TOTAL) {
			return (opType == OpType::READ) ? quota.get().totalReadQuota : quota.get().totalWriteQuota;
		} else {
			return (opType == OpType::READ) ? quota.get().reservedReadQuota : quota.get().reservedWriteQuota;
		}
	}

	// Of all tags meaningfully performing workload on the given storage server,
	// returns the ratio of total quota allocated to the specified tag
	double getQuotaRatio(TransactionTagRef tag, UID storageServerId, OpType opType) const {
		double sumQuota{ 0.0 };
		double tagQuota{ 0.0 };
		auto const tagsAffectingStorageServer = getTagsAffectingStorageServer(storageServerId);
		for (const auto& t : tagsAffectingStorageServer) {
			auto const tQuota = getQuota(t, opType, LimitType::TOTAL);
			sumQuota += tQuota.orDefault(0);
			if (tag.compare(tag) == 0) {
				tagQuota = tQuota.orDefault(0);
			}
		}
		if (tagQuota == 0.0) {
			return 0;
		}
		ASSERT_GT(sumQuota, 0.0);
		return tagQuota / sumQuota;
	}

	// Returns the desired cost for a storage server, based on its current
	// cost and throttling ratio
	Optional<double> getLimitingCost(UID storageServerId, OpType opType) const {
		auto const throttlingRatio = get(throttlingRatios, storageServerId);
		auto const currentCost = getCurrentCost(storageServerId, opType);
		if (!throttlingRatio.present() || currentCost.present() || !throttlingRatio.get().present()) {
			return {};
		}
		return throttlingRatio.get().get() * currentCost.get();
	}

	// For a given storage server and tag combination, return the limiting transaction rate.
	Optional<double> getLimitingTps(UID storageServerId, TransactionTag tag, OpType opType) {
		auto const quotaRatio = getQuotaRatio(tag, storageServerId, opType);
		auto const limitingCost = getLimitingCost(storageServerId, opType);
		auto const averageTransactionCost = getAverageTransactionCost(tag, storageServerId, opType);
		if (!limitingCost.present() || !averageTransactionCost.present()) {
			return {};
		}

		auto const limitingCostForTag = limitingCost.get() * quotaRatio;
		return limitingCostForTag / averageTransactionCost.get();
	}

	// Return the limiting transaction rate, aggregated across all storage servers
	Optional<double> getLimitingTps(TransactionTag tag, OpType opType) {
		Optional<double> result;
		for (const auto& [id, _] : throttlingRatios) {
			auto const targetTpsForSS = getLimitingTps(id, tag, opType);
			if (result.present() && targetTpsForSS.present()) {
				result = std::min(result.get(), targetTpsForSS.get());
			} else {
				result = targetTpsForSS;
			}
		}
		return result;
	}

	Optional<double> getLimitingTps(TransactionTag tag) {
		auto const readLimitingTps = getLimitingTps(tag, OpType::READ);
		auto const writeLimitingTps = getLimitingTps(tag, OpType::WRITE);
		if (readLimitingTps.present() && writeLimitingTps.present()) {
			return std::min(readLimitingTps.get(), writeLimitingTps.get());
		} else if (readLimitingTps.present()) {
			return readLimitingTps;
		} else {
			return writeLimitingTps;
		}
	}

	Optional<double> getDesiredTps(TransactionTag tag, OpType opType) const {
		auto const averageTransactionCost = getAverageTransactionCost(tag, opType);
		if (!averageTransactionCost.present() || averageTransactionCost.get() == 0) {
			return {};
		}

		auto const stats = get(tagStatistics, tag);
		if (!stats.present()) {
			return {};
		}
		auto const quota = stats.get().getQuota();
		if (!quota.present()) {
			return {};
		}
		auto const desiredCost = (opType == OpType::READ) ? quota.get().totalReadQuota : quota.get().totalWriteQuota;
		return desiredCost / averageTransactionCost.get();
	}

	Optional<double> getDesiredTps(TransactionTag tag) const {
		auto const readDesiredTps = getDesiredTps(tag, OpType::READ);
		auto const writeDesiredTps = getDesiredTps(tag, OpType::WRITE);
		if (readDesiredTps.present() && writeDesiredTps.present()) {
			return std::min(readDesiredTps.get(), writeDesiredTps.get());
		} else if (readDesiredTps.present()) {
			return readDesiredTps;
		} else {
			return writeDesiredTps;
		}
	}

	Optional<double> getReservedTps(TransactionTag tag, OpType opType) const {
		auto const reservedCost = getQuota(tag, opType, LimitType::RESERVED);
		auto const averageTransactionCost = getAverageTransactionCost(tag, opType);
		if (!reservedCost.present() || !averageTransactionCost.present() || averageTransactionCost.get() == 0) {
			return {};
		} else {
			return reservedCost.get() / averageTransactionCost.get();
		}
	}

	Optional<double> getReservedTps(TransactionTag tag) const {
		auto const readReservedTps = getReservedTps(tag, OpType::READ);
		auto const writeReservedTps = getReservedTps(tag, OpType::WRITE);
		if (readReservedTps.present() && writeReservedTps.present()) {
			return std::max(readReservedTps.get(), writeReservedTps.get());
		} else if (readReservedTps.present()) {
			return readReservedTps;
		} else {
			return writeReservedTps;
		}
	}

	void removeUnseenTags(std::unordered_set<TransactionTag> const& seenTags) {
		std::unordered_map<TransactionTag, PerTagStatistics>::iterator it = tagStatistics.begin();
		while (it != tagStatistics.end()) {
			auto current = it++;
			auto const tag = current->first;
			if (tagStatistics.find(tag) == tagStatistics.end()) {
				tagStatistics.erase(current);
			}
		}
	}

	ACTOR static Future<Void> monitorThrottlingChanges(GlobalTagThrottlerImpl* self) {
		state std::unordered_set<TransactionTag> seenTags;

		loop {
			state ReadYourWritesTransaction tr(self->db);
			loop {
				try {
					tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

					seenTags.clear();
					state RangeResult currentQuotas = wait(tr.getRange(tagQuotaKeys, CLIENT_KNOBS->TOO_MANY));
					TraceEvent("GlobalTagThrottler_ReadCurrentQuotas").detail("Size", currentQuotas.size());
					for (auto const kv : currentQuotas) {
						auto const tag = kv.key.removePrefix(tagQuotaPrefix);
						auto const quota = ThrottleApi::TagQuotaValue::fromValue(kv.value);
						self->tagStatistics[tag].setQuota(quota);
						seenTags.insert(tag);
					}
					self->removeUnseenTags(seenTags);
					++self->throttledTagChangeId;
					wait(delay(5.0));
					TraceEvent("GlobalTagThrottler_ChangeSignaled");
					CODE_PROBE(true, "Global tag throttler detected quota changes");
					break;
				} catch (Error& e) {
					TraceEvent("GlobalTagThrottlerMonitoringChangesError", self->id).error(e);
					wait(tr.onError(e));
				}
			}
		}
	}

public:
	GlobalTagThrottlerImpl(Database db, UID id) : db(db), id(id) {}
	Future<Void> monitorThrottlingChanges() { return monitorThrottlingChanges(this); }
	void addRequests(TransactionTag tag, int count) { tagStatistics[tag].addTransactions(count); }
	uint64_t getThrottledTagChangeId() const { return throttledTagChangeId; }
	PrioritizedTransactionTagMap<ClientTagThrottleLimits> getClientRates() {
		PrioritizedTransactionTagMap<ClientTagThrottleLimits> result;
		for (auto& [tag, stats] : tagStatistics) {
			// Currently there is no differentiation between batch priority and default priority transactions
			auto const limitingTps = getLimitingTps(tag);
			auto const desiredTps = getDesiredTps(tag);
			auto const reservedTps = getReservedTps(tag);
			if (!limitingTps.present() || !desiredTps.present() || !reservedTps.present()) {
				return {};
			} else {
				auto const targetCost = std::max(reservedTps.get(), std::min(limitingTps.get(), desiredTps.get()));
				auto const perClientLimit = stats.updateAndGetPerClientLimit(targetCost);
				result[TransactionPriority::BATCH][tag] = result[TransactionPriority::DEFAULT][tag] =
				    perClientLimit.get();
			}
		}
		return result;
	}
	// FIXME: Only count tags that have quota set
	int64_t autoThrottleCount() const { return tagStatistics.size(); }
	uint32_t busyReadTagCount() const {
		// TODO: Implement
		return 0;
	}
	uint32_t busyWriteTagCount() const {
		// TODO: Implement
		return 0;
	}
	int64_t manualThrottleCount() const { return 0; }

	Future<Void> tryUpdateAutoThrottling(StorageQueueInfo const& ss) {
		for (const auto& busyReadTag : ss.busiestReadTags) {
			throughput[ss.id][busyReadTag.tag].updateCost(busyReadTag.rate, OpType::READ);
		}
		for (const auto& busyWriteTag : ss.busiestWriteTags) {
			throughput[ss.id][busyWriteTag.tag].updateCost(busyWriteTag.rate, OpType::WRITE);
		}
		return Void();
	}

	void setThrottlingRatio(UID storageServerId, Optional<double> ratio) { throttlingRatios[storageServerId] = ratio; }

	void setQuota(TransactionTagRef tag, ThrottleApi::TagQuotaValue const& tagQuotaValue) {
		tagStatistics[tag].setQuota(tagQuotaValue);
	}

	void removeQuota(TransactionTagRef tag) { tagStatistics[tag].clearQuota(); }
};

GlobalTagThrottler::GlobalTagThrottler(Database db, UID id) : impl(PImpl<GlobalTagThrottlerImpl>::create(db, id)) {}

GlobalTagThrottler::~GlobalTagThrottler() = default;

Future<Void> GlobalTagThrottler::monitorThrottlingChanges() {
	return impl->monitorThrottlingChanges();
}
void GlobalTagThrottler::addRequests(TransactionTag tag, int count) {
	return impl->addRequests(tag, count);
}
uint64_t GlobalTagThrottler::getThrottledTagChangeId() const {
	return impl->getThrottledTagChangeId();
}
PrioritizedTransactionTagMap<ClientTagThrottleLimits> GlobalTagThrottler::getClientRates() {
	return impl->getClientRates();
}
int64_t GlobalTagThrottler::autoThrottleCount() const {
	return impl->autoThrottleCount();
}
uint32_t GlobalTagThrottler::busyReadTagCount() const {
	return impl->busyReadTagCount();
}
uint32_t GlobalTagThrottler::busyWriteTagCount() const {
	return impl->busyWriteTagCount();
}
int64_t GlobalTagThrottler::manualThrottleCount() const {
	return impl->manualThrottleCount();
}
bool GlobalTagThrottler::isAutoThrottlingEnabled() const {
	return true;
}
Future<Void> GlobalTagThrottler::tryUpdateAutoThrottling(StorageQueueInfo const& ss) {
	return impl->tryUpdateAutoThrottling(ss);
}

void GlobalTagThrottler::setThrottlingRatio(UID storageServerId, Optional<double> ratio) {
	return impl->setThrottlingRatio(storageServerId, ratio);
}

void GlobalTagThrottler::setQuota(TransactionTagRef tag, ThrottleApi::TagQuotaValue const& tagQuotaValue) {
	return impl->setQuota(tag, tagQuotaValue);
}

void GlobalTagThrottler::removeQuota(TransactionTagRef tag) {
	return impl->removeQuota(tag);
}

namespace GlobalTagThrottlerTesting {

Optional<double> getTPSLimit(GlobalTagThrottler& globalTagThrottler, TransactionTag tag) {
	auto clientRates = globalTagThrottler.getClientRates();
	auto it1 = clientRates.find(TransactionPriority::DEFAULT);
	if (it1 != clientRates.end()) {
		auto it2 = it1->second.find(tag);
		if (it2 != it1->second.end()) {
			return it2->second.tpsRate;
		}
	}
	return {};
}

class MockStorageServer {
	class Cost {
		Smoother smoother;

	public:
		Cost() : smoother(5.0) {}
		Cost& operator+=(double delta) {
			smoother.addDelta(delta);
			return *this;
		}
		double smoothRate() const { return smoother.smoothRate(); }
	};

	UID id;
	double targetCostRate;
	std::map<TransactionTag, Cost> readCosts, writeCosts;
	Cost totalReadCost, totalWriteCost;

public:
	explicit MockStorageServer(UID id, double targetCostRate) : id(id), targetCostRate(targetCostRate) {
		ASSERT_GT(targetCostRate, 0);
	}
	void addReadCost(TransactionTag tag, double cost) {
		readCosts[tag] += cost;
		totalReadCost += cost;
	}
	void addWriteCost(TransactionTag tag, double cost) {
		writeCosts[tag] += cost;
		totalWriteCost += cost;
	}

	StorageQueueInfo getStorageQueueInfo() const {
		StorageQueueInfo result(id, LocalityData{});
		for (const auto& [tag, readCost] : readCosts) {
			double fractionalBusyness{ 0.0 }; // unused for global tag throttling
			result.busiestReadTags.emplace_back(tag, readCost.smoothRate(), fractionalBusyness);
		}
		for (const auto& [tag, writeCost] : writeCosts) {
			double fractionalBusyness{ 0.0 }; // unused for global tag throttling
			result.busiestWriteTags.emplace_back(tag, writeCost.smoothRate(), fractionalBusyness);
		}
		return result;
	}

	Optional<double> getThrottlingRatio() const {
		auto const springCostRate = 0.2 * targetCostRate;
		auto const currentCostRate = totalReadCost.smoothRate() + totalWriteCost.smoothRate();
		if (currentCostRate < targetCostRate - springCostRate) {
			return {};
		} else {
			return std::max(0.0, ((targetCostRate + springCostRate) - currentCostRate) / springCostRate);
		}
	}
};

class StorageServerCollection {
	std::vector<MockStorageServer> storageServers;

public:
	StorageServerCollection(size_t size, double targetCostRate) {
		ASSERT_GT(size, 0);
		storageServers.reserve(size);
		for (int i = 0; i < size; ++i) {
			storageServers.emplace_back(UID(i, i), targetCostRate);
		}
	}

	void addReadCost(TransactionTag tag, double cost) {
		auto const costPerSS = cost / storageServers.size();
		for (auto& storageServer : storageServers) {
			storageServer.addReadCost(tag, costPerSS);
		}
	}

	void addWriteCost(TransactionTag tag, double cost) {
		auto const costPerSS = cost / storageServers.size();
		for (auto& storageServer : storageServers) {
			storageServer.addWriteCost(tag, costPerSS);
		}
	}

	std::vector<StorageQueueInfo> getStorageQueueInfos() const {
		std::vector<StorageQueueInfo> result;
		result.reserve(storageServers.size());
		for (const auto& storageServer : storageServers) {
			result.push_back(storageServer.getStorageQueueInfo());
		}
		return result;
	}

	std::map<UID, Optional<double>> getThrottlingRatios() const {
		std::map<UID, Optional<double>> result;
		for (int i = 0; i < storageServers.size(); ++i) {
			result[UID(i, i)] = storageServers[i].getThrottlingRatio();
		}
		return result;
	}
};

ACTOR static Future<Void> runClient(GlobalTagThrottler* globalTagThrottler,
                                    StorageServerCollection* storageServers,
                                    TransactionTag tag,
                                    double desiredTpsRate,
                                    double costPerTransaction,
                                    bool write) {
	loop {
		auto tpsLimit = getTPSLimit(*globalTagThrottler, tag);
		state double tpsRate = tpsLimit.present() ? std::min<double>(desiredTpsRate, tpsLimit.get()) : desiredTpsRate;
		wait(delay(1 / tpsRate));
		if (write) {
			storageServers->addWriteCost(tag, costPerTransaction);
		} else {
			storageServers->addReadCost(tag, costPerTransaction);
		}
		globalTagThrottler->addRequests(tag, 1);
	}
}

ACTOR static Future<Void> monitorClientRates(GlobalTagThrottler* globalTagThrottler,
                                             TransactionTag tag,
                                             Optional<double> desiredTPSLimit) {
	state int successes = 0;
	loop {
		wait(delay(1.0));
		auto currentTPSLimit = getTPSLimit(*globalTagThrottler, tag);
		if (currentTPSLimit.present()) {
			TraceEvent("GlobalTagThrottling_RateMonitor")
			    .detail("Tag", tag)
			    .detail("CurrentTPSRate", currentTPSLimit.get())
			    .detail("DesiredTPSRate", desiredTPSLimit);
			if (desiredTPSLimit.present() && abs(currentTPSLimit.get() - desiredTPSLimit.get()) < 1.0) {
				if (++successes == 3) {
					return Void();
				}
			} else {
				successes = 0;
			}
		} else {
			TraceEvent("GlobalTagThrottling_RateMonitor")
			    .detail("Tag", tag)
			    .detail("CurrentTPSRate", currentTPSLimit)
			    .detail("DesiredTPSRate", desiredTPSLimit);
			if (desiredTPSLimit.present()) {
				successes = 0;
			} else {
				if (++successes == 3) {
					return Void();
				}
			}
		}
	}
}

ACTOR static Future<Void> updateGlobalTagThrottler(GlobalTagThrottler* globalTagThrottler,
                                                   StorageServerCollection const* storageServers) {
	loop {
		wait(delay(1.0));
		auto const storageQueueInfos = storageServers->getStorageQueueInfos();
		for (const auto& sq : storageQueueInfos) {
			globalTagThrottler->tryUpdateAutoThrottling(sq);
		}
		auto const throttlingRatios = storageServers->getThrottlingRatios();
		for (const auto& [id, ratio] : throttlingRatios) {
			globalTagThrottler->setThrottlingRatio(id, ratio);
		}
	}
}

} // namespace GlobalTagThrottlerTesting

TEST_CASE("/GlobalTagThrottler/Simple") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 100);
	ThrottleApi::TagQuotaValue tagQuotaValue;
	TransactionTag testTag = "sampleTag1"_sr;
	tagQuotaValue.totalReadQuota = 100.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	state Future<Void> client =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 5.0, 6.0, false);
	state Future<Void> monitor =
	    GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 100.0 / 6.0);
	state Future<Void> updater =
	    GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers);
	wait(timeoutError(monitor || client || updater, 300.0));
	return Void();
}

TEST_CASE("/GlobalTagThrottler/WriteThrottling") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 100);
	ThrottleApi::TagQuotaValue tagQuotaValue;
	TransactionTag testTag = "sampleTag1"_sr;
	tagQuotaValue.totalWriteQuota = 100.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	state Future<Void> client =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 5.0, 6.0, true);
	state Future<Void> monitor =
	    GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 100.0 / 6.0);
	state Future<Void> updater =
	    GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers);
	wait(timeoutError(monitor || client || updater, 300.0));
	return Void();
}

TEST_CASE("/GlobalTagThrottler/MultiTagThrottling") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 100);
	ThrottleApi::TagQuotaValue tagQuotaValue;
	TransactionTag testTag1 = "sampleTag1"_sr;
	TransactionTag testTag2 = "sampleTag2"_sr;
	tagQuotaValue.totalReadQuota = 100.0;
	globalTagThrottler.setQuota(testTag1, tagQuotaValue);
	globalTagThrottler.setQuota(testTag2, tagQuotaValue);
	state std::vector<Future<Void>> futures;
	state std::vector<Future<Void>> monitorFutures;
	futures.push_back(
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag1, 5.0, 6.0, false));
	futures.push_back(
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag2, 5.0, 6.0, false));
	futures.push_back(GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers));
	monitorFutures.push_back(GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag1, 100.0 / 6.0));
	monitorFutures.push_back(GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag2, 100.0 / 6.0));
	wait(timeoutError(waitForAny(futures) || waitForAll(monitorFutures), 300.0));
	return Void();
}

TEST_CASE("/GlobalTagThrottler/AttemptWorkloadAboveQuota") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 100);
	ThrottleApi::TagQuotaValue tagQuotaValue;
	TransactionTag testTag = "sampleTag1"_sr;
	tagQuotaValue.totalReadQuota = 100.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	state Future<Void> client =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 20.0, 10.0, false);
	state Future<Void> monitor = GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 10.0);
	state Future<Void> updater =
	    GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers);
	wait(timeoutError(monitor || client || updater, 300.0));
	return Void();
}

TEST_CASE("/GlobalTagThrottler/MultiClientThrottling") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 100);
	ThrottleApi::TagQuotaValue tagQuotaValue;
	TransactionTag testTag = "sampleTag1"_sr;
	tagQuotaValue.totalReadQuota = 100.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	state Future<Void> client =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 5.0, 6.0, false);
	state Future<Void> client2 =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 5.0, 6.0, false);
	state Future<Void> monitor =
	    GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 100.0 / 6.0);
	state Future<Void> updater =
	    GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers);
	wait(timeoutError(monitor || client || updater, 300.0));
	return Void();
}

TEST_CASE("/GlobalTagThrottler/MultiClientActiveThrottling") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 100);
	ThrottleApi::TagQuotaValue tagQuotaValue;
	TransactionTag testTag = "sampleTag1"_sr;
	tagQuotaValue.totalReadQuota = 100.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	state Future<Void> client =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 20.0, 10.0, false);
	state Future<Void> client2 =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 20.0, 10.0, false);
	state Future<Void> monitor = GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 5.0);
	state Future<Void> updater =
	    GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers);
	wait(timeoutError(monitor || client || updater, 300.0));
	return Void();
}

// Global transaction rate should be 20.0, with a distribution of (5, 15) between the 2 clients
TEST_CASE("/GlobalTagThrottler/SkewedMultiClientActiveThrottling") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 100);
	ThrottleApi::TagQuotaValue tagQuotaValue;
	TransactionTag testTag = "sampleTag1"_sr;
	tagQuotaValue.totalReadQuota = 100.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	state Future<Void> client =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 5.0, 5.0, false);
	state Future<Void> client2 =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 25.0, 5.0, false);
	state Future<Void> monitor = GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 15.0);
	state Future<Void> updater =
	    GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers);
	wait(timeoutError(monitor || client || updater, 300.0));
	return Void();
}

// Test that the tag throttler can reach equilibrium, then adjust to a new equilibrium once the quota is changed
TEST_CASE("/GlobalTagThrottler/UpdateQuota") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 100);
	state ThrottleApi::TagQuotaValue tagQuotaValue;
	state TransactionTag testTag = "sampleTag1"_sr;
	tagQuotaValue.totalReadQuota = 100.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	state Future<Void> client =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 5.0, 6.0, false);
	state Future<Void> monitor =
	    GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 100.0 / 6.0);
	state Future<Void> updater =
	    GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers);
	wait(timeoutError(monitor || client || updater, 300.0));
	tagQuotaValue.totalReadQuota = 50.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	monitor = GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 50.0 / 6.0);
	wait(timeoutError(monitor || client || updater, 300.0));
	return Void();
}

TEST_CASE("/GlobalTagThrottler/RemoveQuota") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 100);
	state ThrottleApi::TagQuotaValue tagQuotaValue;
	state TransactionTag testTag = "sampleTag1"_sr;
	tagQuotaValue.totalReadQuota = 100.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	state Future<Void> client =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 5.0, 6.0, false);
	state Future<Void> monitor =
	    GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 100.0 / 6.0);
	state Future<Void> updater =
	    GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers);
	wait(timeoutError(monitor || client || updater, 300.0));
	globalTagThrottler.removeQuota(testTag);
	monitor = GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, {});
	wait(timeoutError(monitor || client || updater, 300.0));
	return Void();
}

TEST_CASE("/GlobalTagThrottler/ActiveThrottling") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 5);
	state ThrottleApi::TagQuotaValue tagQuotaValue;
	state TransactionTag testTag = "sampleTag1"_sr;
	tagQuotaValue.totalReadQuota = 100.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	state Future<Void> client =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 10.0, 6.0, false);
	state Future<Void> monitor = GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 50 / 6.0);
	state Future<Void> updater =
	    GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers);
	wait(timeoutError(monitor || client || updater, 300.0));
	return Void();
}

TEST_CASE("/GlobalTagThrottler/MultiTagActiveThrottling") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 5);
	state ThrottleApi::TagQuotaValue tagQuotaValue1;
	state ThrottleApi::TagQuotaValue tagQuotaValue2;
	state TransactionTag testTag1 = "sampleTag1"_sr;
	state TransactionTag testTag2 = "sampleTag2"_sr;
	tagQuotaValue1.totalReadQuota = 50.0;
	tagQuotaValue2.totalReadQuota = 100.0;
	globalTagThrottler.setQuota(testTag1, tagQuotaValue1);
	globalTagThrottler.setQuota(testTag2, tagQuotaValue2);
	std::vector<Future<Void>> futures;
	futures.push_back(
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag1, 10.0, 6.0, false));
	futures.push_back(
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag2, 10.0, 6.0, false));
	Future<Void> monitor1 =
	    GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag1, (50 / 6.0) / 3);
	Future<Void> monitor2 =
	    GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag2, 2 * (50 / 6.0) / 3);
	futures.push_back(GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers));
	wait(timeoutError(waitForAny(futures) || (monitor1 && monitor2), 300.0));
	return Void();
}

TEST_CASE("/GlobalTagThrottler/ReservedReadQuota") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 5);
	state ThrottleApi::TagQuotaValue tagQuotaValue;
	state TransactionTag testTag = "sampleTag1"_sr;
	tagQuotaValue.totalReadQuota = 100.0;
	tagQuotaValue.reservedReadQuota = 70.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	state Future<Void> client =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 10.0, 6.0, false);
	state Future<Void> monitor = GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 70 / 6.0);
	state Future<Void> updater =
	    GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers);
	wait(timeoutError(monitor || client || updater, 300.0));
	return Void();
}

TEST_CASE("/GlobalTagThrottler/ReservedWriteQuota") {
	state GlobalTagThrottler globalTagThrottler(Database{}, UID{});
	state GlobalTagThrottlerTesting::StorageServerCollection storageServers(10, 5);
	state ThrottleApi::TagQuotaValue tagQuotaValue;
	state TransactionTag testTag = "sampleTag1"_sr;
	tagQuotaValue.totalWriteQuota = 100.0;
	tagQuotaValue.reservedWriteQuota = 70.0;
	globalTagThrottler.setQuota(testTag, tagQuotaValue);
	state Future<Void> client =
	    GlobalTagThrottlerTesting::runClient(&globalTagThrottler, &storageServers, testTag, 10.0, 6.0, true);
	state Future<Void> monitor = GlobalTagThrottlerTesting::monitorClientRates(&globalTagThrottler, testTag, 70 / 6.0);
	state Future<Void> updater =
	    GlobalTagThrottlerTesting::updateGlobalTagThrottler(&globalTagThrottler, &storageServers);
	wait(timeoutError(monitor || client || updater, 300.0));
	return Void();
}
