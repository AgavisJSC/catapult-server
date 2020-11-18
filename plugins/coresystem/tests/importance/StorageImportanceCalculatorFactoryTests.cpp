/**
*** Copyright (c) 2016-present,
*** Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp. All rights reserved.
***
*** This file is part of Catapult.
***
*** Catapult is free software: you can redistribute it and/or modify
*** it under the terms of the GNU Lesser General Public License as published by
*** the Free Software Foundation, either version 3 of the License, or
*** (at your option) any later version.
***
*** Catapult is distributed in the hope that it will be useful,
*** but WITHOUT ANY WARRANTY; without even the implied warranty of
*** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*** GNU Lesser General Public License for more details.
***
*** You should have received a copy of the GNU Lesser General Public License
*** along with Catapult. If not, see <http://www.gnu.org/licenses/>.
**/

#include "src/importance/StorageImportanceCalculatorFactory.h"
#include "catapult/cache_core/AccountStateCache.h"
#include "catapult/io/IndexFile.h"
#include "catapult/model/BlockChainConfiguration.h"
#include "tests/test/cache/AccountStateCacheTestUtils.h"
#include "tests/test/nodeps/Filesystem.h"
#include "tests/TestHarness.h"

namespace catapult { namespace importance {

#define TEST_CLASS StorageImportanceCalculatorFactoryTests

	namespace {
		constexpr auto Harvesting_Mosaic_Id = MosaicId(2222);
		constexpr auto Importance_Grouping = static_cast<uint32_t>(123);
		constexpr auto Default_Calculation_Mode = ImportanceRollbackMode::Enabled;

		// region structs

#pragma pack(push, 1)

		struct FileHeader {
			uint16_t Version;
			uint64_t Count;
		};

		struct AccountHeader {
			catapult::Address Address;
			Amount Balance;
		};

		struct AccountImportanceSeed {
			catapult::Importance Importance;

			Amount TotalFeesPaid;
			uint32_t BeneficiaryCount;
			uint64_t RawScore;
		};

#pragma pack(pop)

		// endregion

		// region MockImportanceCalculator

		class MockImportanceCalculator final : public ImportanceCalculator {
		public:
			using RecalculateMap = std::map<std::pair<Address, model::ImportanceHeight>, AccountImportanceSeed>;

		public:
			explicit MockImportanceCalculator(const RecalculateMap& map) : m_map(map)
			{}

		public:
			void recalculate(
					ImportanceRollbackMode mode,
					model::ImportanceHeight importanceHeight,
					cache::AccountStateCacheDelta& cache) const override {
				const auto& highValueAddresses = cache.highValueAccounts().addresses();
				for (const auto& address : highValueAddresses) {
					auto accountStateIter = cache.find(address);
					auto mapIter = m_map.find({ address, importanceHeight });
					if (m_map.cend() == mapIter) {
						std::ostringstream out;
						out << "unexpected recalculate call for " << address << " at " << importanceHeight;
						CATAPULT_THROW_RUNTIME_ERROR(out.str().c_str());
					}

					// simulate the real PosImportanceCalculator that does the following:
					// 1. sets importance at `importanceHeight`
					// 2. finalizes activity bucket at previous `importanceHeight`
					// 3. creates empty activity bucket at `importanceHeight`

					// add mode to configured importance to provide indirect way of checking correct mode was passed
					auto importance = mapIter->second.Importance + Importance(static_cast<Importance::ValueType>(mode));
					accountStateIter.get().ImportanceSnapshots.set(importance, importanceHeight);

					auto groupingFacade = model::HeightGroupingFacade<model::ImportanceHeight>(importanceHeight, Importance_Grouping);
					accountStateIter.get().ActivityBuckets.update(groupingFacade.previous(1), [&seed = mapIter->second](auto& bucket) {
						bucket.TotalFeesPaid = seed.TotalFeesPaid;
						bucket.BeneficiaryCount = seed.BeneficiaryCount;
						bucket.RawScore = seed.RawScore;
					});

					accountStateIter.get().ActivityBuckets.update(importanceHeight, [](const auto&) {});
				}
			}

		private:
			RecalculateMap m_map;
		};

		// endregion

		// region CacheHolder

		class CacheHolder {
		private:
			using ImportanceSnapshot = state::AccountImportanceSnapshots::ImportanceSnapshot;
			using ActivityBucket = state::AccountActivityBuckets::ActivityBucket;

		public:
			CacheHolder()
					: m_cache(
							cache::CacheConfiguration(),
							test::CreateDefaultAccountStateCacheOptions(MosaicId(1111), Harvesting_Mosaic_Id))
					, m_delta(m_cache.createDelta())
			{}

		public:
			auto& get() {
				return *m_delta;
			}

		public:
			void addAccounts(const std::vector<Address>& addresses, const std::vector<Amount>& balances) {
				auto i = 0u;
				for (const auto& address : addresses) {
					m_delta->addAccount(address, Height(1));
					m_delta->find(address).get().Balances.credit(Harvesting_Mosaic_Id, balances[i]);
					++i;
				}

				m_delta->updateHighValueAccounts(Height(1));
			}

			std::vector<Address> addAccounts(const std::vector<Amount>& balances) {
				auto addresses = test::GenerateRandomDataVector<Address>(balances.size());
				addAccounts(addresses, balances);
				return addresses;
			}

		public:
			void assertEqualImportances(
					const Address& address,
					size_t id,
					const std::vector<ImportanceSnapshot>& expected,
					const std::string& postfix = std::string()) const {
				auto message = "account at " + std::to_string(id) + " importance" + postfix;
				AssertEqual(expected, m_delta->find(address).get().ImportanceSnapshots, message);
			}

			void assertEqualActivityBuckets(
					const Address& address,
					size_t id,
					const std::vector<ActivityBucket>& expected,
					const std::string& postfix = std::string()) const {
				auto message = "account at " + std::to_string(id) + " activity bucket" + postfix;
				AssertEqual(expected, m_delta->find(address).get().ActivityBuckets, message);
			}

		private:
			static void AssertEqual(
					const std::vector<ImportanceSnapshot>& expected,
					const state::AccountImportanceSnapshots& actual,
					const std::string& message) {
				auto i = 0u;
				for (const auto& actualSnapshot : actual) {
					const auto& expectedSnapshot = i >= expected.size() ? ImportanceSnapshot() : expected[i];
					EXPECT_EQ(expectedSnapshot.Importance, actualSnapshot.Importance) << message << " at " << i;
					EXPECT_EQ(expectedSnapshot.Height, actualSnapshot.Height) << message << " at " << i;
					++i;
				}
			}

			static void AssertEqual(
					const std::vector<ActivityBucket>& expected,
					const state::AccountActivityBuckets& actual,
					const std::string& message) {
				auto i = 0u;
				for (const auto& actualBucket : actual) {
					const auto& expectedBucket = i >= expected.size() ? ActivityBucket() : expected[i];
					EXPECT_EQ(expectedBucket.StartHeight, actualBucket.StartHeight) << message << " at " << i;
					EXPECT_EQ(expectedBucket.TotalFeesPaid, actualBucket.TotalFeesPaid) << message << " at " << i;
					EXPECT_EQ(expectedBucket.BeneficiaryCount, actualBucket.BeneficiaryCount) << message << " at " << i;
					EXPECT_EQ(expectedBucket.RawScore, actualBucket.RawScore) << message << " at " << i;
					++i;
				}
			}

		private:
			cache::AccountStateCache m_cache;
			cache::LockedCacheDelta<cache::AccountStateCacheDelta> m_delta;
		};

		// endregion

		// region TestContext

		class TestContext {
		public:
			TestContext() : m_config(CreateBlockChainConfiguration())
			{}

		public:
			auto createWriteCalculator(const MockImportanceCalculator::RecalculateMap& map) const {
				return factory().createWriteCalculator(std::make_unique<MockImportanceCalculator>(map));
			}

			auto createReadCalculator(const MockImportanceCalculator::RecalculateMap& map) const {
				return factory().createReadCalculator(std::make_unique<MockImportanceCalculator>(map));
			}

		public:
			uint64_t loadIndexValue() {
				return indexFile().get();
			}

			void setIndexValue(uint64_t value) {
				indexFile().set(value);
			}

			size_t fileSize(const std::string& filename) {
				return std::filesystem::file_size(qualify(filename));
			}

			std::vector<uint8_t> readAll(const std::string& filename) {
				io::RawFile rawFile(qualify(filename), io::OpenMode::Read_Only);

				std::vector<uint8_t> contents(rawFile.size());
				rawFile.read(contents);
				return contents;
			}

		public:
			void assertFiles(const std::unordered_set<std::string>& expectedFilenames) {
				EXPECT_EQ(1 + expectedFilenames.size(), test::CountFilesAndDirectories(m_tempDir.name()));

				for (const auto& filename : expectedFilenames)
					EXPECT_TRUE(std::filesystem::exists(qualify(filename))) << filename;
			}

			void assertNoFiles() {
				EXPECT_EQ(0u, test::CountFilesAndDirectories(m_tempDir.name()));
				EXPECT_FALSE(indexFile().exists());
			}

		private:
			std::string qualify(const std::string& filename) const {
				return (std::filesystem::path(m_tempDir.name()) / filename).generic_string();
			}

			io::IndexFile indexFile() {
				return io::IndexFile(qualify("index.dat"));
			}

			StorageImportanceCalculatorFactory factory() const {
				return StorageImportanceCalculatorFactory(m_config, config::CatapultDirectory(m_tempDir.name()));
			}

		private:
			static model::BlockChainConfiguration CreateBlockChainConfiguration() {
				auto config = model::BlockChainConfiguration::Uninitialized();
				config.HarvestingMosaicId = Harvesting_Mosaic_Id;
				config.ImportanceGrouping = Importance_Grouping;
				return config;
			}

		private:
			test::TempDirectoryGuard m_tempDir;
			model::BlockChainConfiguration m_config;
		};

		// endregion

		// region FileContentsBuilder

		class FileContentsBuilder {
		private:
			static constexpr auto Account_Part_Size = sizeof(AccountHeader) + sizeof(AccountImportanceSeed);

		public:
			explicit FileContentsBuilder(const FileHeader& header) {
				m_buffer.resize(sizeof(FileHeader));
				std::memcpy(&m_buffer[0], &header, sizeof(FileHeader));
			}

		public:
			void append(const AccountHeader& header, const AccountImportanceSeed& seed) {
				auto offset = m_buffer.size();
				m_buffer.resize(offset + sizeof(AccountHeader) + sizeof(AccountImportanceSeed));
				std::memcpy(&m_buffer[offset], &header, sizeof(AccountHeader));
				std::memcpy(&m_buffer[offset + sizeof(AccountHeader)], &seed, sizeof(AccountImportanceSeed));
			}

		public:
			void assertContents(const std::vector<uint8_t>& actual) const {
				// Assert:
				ASSERT_EQ(m_buffer.size(), actual.size());

				// - check header
				EXPECT_EQ_MEMORY(&m_buffer[0], &actual[0], sizeof(FileHeader));

				// - check files (can be out of order)
				std::set<std::vector<uint8_t>> actualSplitAccountParts;
				std::set<std::vector<uint8_t>> expectedSplitAccountParts;
				for (auto offset = sizeof(FileHeader); offset < m_buffer.size(); offset += Account_Part_Size) {
					actualSplitAccountParts.emplace(&actual[offset], &actual[offset] + Account_Part_Size);
					expectedSplitAccountParts.emplace(&m_buffer[offset], &m_buffer[offset] + Account_Part_Size);
				}

				EXPECT_EQ(expectedSplitAccountParts, actualSplitAccountParts);
			}

		private:
			std::vector<uint8_t> m_buffer;
		};

		// endregion
	}

	// region createWriteCalculator

	namespace {
		void AssertWriteCalculatorDelegatesToDecoratedCalculator(ImportanceRollbackMode mode, Importance importanceAdjustment) {
			// Arrange:
			static constexpr auto Previous_Importance_Height = model::ImportanceHeight(123);
			static constexpr auto Importance_Height = model::ImportanceHeight(246);

			TestContext context;
			CacheHolder holder;
			auto addresses = holder.addAccounts({ Amount(2000), Amount(1000), Amount(3000) });

			// Sanity:
			for (auto i = 0u; i < addresses.size(); ++i) {
				holder.assertEqualImportances(addresses[i], i, {}, " (sanity zero)");
				holder.assertEqualActivityBuckets(addresses[i], i, {}, " (sanity zero)");
			}

			// Act:
			auto pCalculator = context.createWriteCalculator({
				{ { addresses[0], Importance_Height }, { Importance(111), Amount(222), 14, 400 } },
				{ { addresses[1], Importance_Height }, { Importance(123), Amount(432), 22, 200 } },
				{ { addresses[2], Importance_Height }, { Importance(444), Amount(110), 10, 300 } }
			});
			pCalculator->recalculate(mode, Importance_Height, holder.get());

			// Assert:
			holder.assertEqualImportances(addresses[0], 0, { { Importance(111) + importanceAdjustment, Importance_Height } });
			holder.assertEqualImportances(addresses[1], 1, { { Importance(123) + importanceAdjustment, Importance_Height } });
			holder.assertEqualImportances(addresses[2], 2, { { Importance(444) + importanceAdjustment, Importance_Height } });

			holder.assertEqualActivityBuckets(addresses[0], 0, {
				{ { Amount(0), 0, 0 }, Importance_Height },
				{ { Amount(222), 14, 400 }, Previous_Importance_Height }
			});
			holder.assertEqualActivityBuckets(addresses[1], 1, {
				{ { Amount(0), 0, 0 }, Importance_Height },
				{ { Amount(432), 22, 200 }, Previous_Importance_Height }
			});
			holder.assertEqualActivityBuckets(addresses[2], 2, {
				{ { Amount(0), 0, 0 }, Importance_Height },
				{ { Amount(110), 10, 300 }, Previous_Importance_Height }
			});
		}
	}

	TEST(TEST_CLASS, WriteCalculatorDelegatesToDecoratedCalculator) {
		AssertWriteCalculatorDelegatesToDecoratedCalculator(Default_Calculation_Mode, Importance(0));
	}

	TEST(TEST_CLASS, WriteCalculatorCanWriteSingleImportanceFile) {
		// Arrange:
		static constexpr auto Importance_Height = model::ImportanceHeight(246);
		static constexpr auto Importance_Filename = "00000000000000F6.dat";

		TestContext context;
		CacheHolder holder;
		auto addresses = holder.addAccounts({ Amount(2000), Amount(1000), Amount(3000) });

		// Act:
		auto pCalculator = context.createWriteCalculator({
			{ { addresses[0], Importance_Height }, { Importance(111), Amount(222), 14, 400 } },
			{ { addresses[1], Importance_Height }, { Importance(123), Amount(432), 22, 200 } },
			{ { addresses[2], Importance_Height }, { Importance(444), Amount(110), 10, 300 } }
		});
		pCalculator->recalculate(Default_Calculation_Mode, Importance_Height, holder.get());

		// Assert:
		EXPECT_EQ(Importance_Height.unwrap(), context.loadIndexValue());
		context.assertFiles({ Importance_Filename });

		FileContentsBuilder contentsBuilder({ 1, 3 });
		contentsBuilder.append({ addresses[0], Amount(2000) }, { Importance(111), Amount(222), 14, 400 });
		contentsBuilder.append({ addresses[1], Amount(1000) }, { Importance(123), Amount(432), 22, 200 });
		contentsBuilder.append({ addresses[2], Amount(3000) }, { Importance(444), Amount(110), 10, 300 });
		contentsBuilder.assertContents(context.readAll(Importance_Filename));
	}

	TEST(TEST_CLASS, WriteCalculatorCanOverwriteFile) {
		// Arrange:
		static constexpr auto Importance_Height = model::ImportanceHeight(246);
		static constexpr auto Importance_Filename = "00000000000000F6.dat";

		TestContext context;
		CacheHolder holder1;
		auto addresses1 = holder1.addAccounts({ Amount(8000), Amount(2000), Amount(9000) });
		auto addresses2 = test::GenerateRandomDataVector<Address>(1);

		// - write file initially
		auto pCalculator = context.createWriteCalculator({
			{ { addresses1[0], Importance_Height }, { Importance(111), Amount(222), 14, 400 } },
			{ { addresses1[1], Importance_Height }, { Importance(123), Amount(432), 22, 200 } },
			{ { addresses1[2], Importance_Height }, { Importance(444), Amount(110), 10, 300 } },
			{ { addresses2[0], Importance_Height }, { Importance(555), Amount(678), 18, 100 } }
		});
		pCalculator->recalculate(Default_Calculation_Mode, Importance_Height, holder1.get());

		// Sanity:
		EXPECT_EQ(sizeof(FileHeader) + 3 * (sizeof(AccountHeader) + sizeof(AccountImportanceSeed)), context.fileSize(Importance_Filename));

		// Act: add another account and recalculate (use a fresh cache to avoid setting multiple importances at same height)
		CacheHolder holder2;
		holder2.addAccounts(addresses1, { Amount(2000), Amount(1000), Amount(3000) });
		holder2.addAccounts({ addresses2[0] }, { Amount(4000) });
		pCalculator->recalculate(Default_Calculation_Mode, Importance_Height, holder2.get());

		// Assert:
		EXPECT_EQ(Importance_Height.unwrap(), context.loadIndexValue());
		context.assertFiles({ Importance_Filename });

		FileContentsBuilder contentsBuilder({ 1, 4 });
		contentsBuilder.append({ addresses1[0], Amount(2000) }, { Importance(111), Amount(222), 14, 400 });
		contentsBuilder.append({ addresses1[1], Amount(1000) }, { Importance(123), Amount(432), 22, 200 });
		contentsBuilder.append({ addresses1[2], Amount(3000) }, { Importance(444), Amount(110), 10, 300 });
		contentsBuilder.append({ addresses2[0], Amount(4000) }, { Importance(555), Amount(678), 18, 100 });
		contentsBuilder.assertContents(context.readAll(Importance_Filename));
	}

	// endregion

	// region createWriteCalculator (RollbackDisabled)

	TEST(TEST_CLASS, WriteCalculatorDelegatesToDecoratedCalculator_RollbackDisabled) {
		AssertWriteCalculatorDelegatesToDecoratedCalculator(ImportanceRollbackMode::Disabled, Importance(1));
	}

	TEST(TEST_CLASS, WriteCalculatorDoesNotWriteFiles_RollbackDisabled) {
		// Arrange:
		static constexpr auto Importance_Height = model::ImportanceHeight(246);

		TestContext context;
		CacheHolder holder;
		auto addresses = holder.addAccounts({ Amount(2000), Amount(1000), Amount(3000) });

		// Act:
		auto pCalculator = context.createWriteCalculator({
			{ { addresses[0], Importance_Height }, { Importance(111), Amount(222), 14, 400 } },
			{ { addresses[1], Importance_Height }, { Importance(123), Amount(432), 22, 200 } },
			{ { addresses[2], Importance_Height }, { Importance(444), Amount(110), 10, 300 } }
		});
		pCalculator->recalculate(ImportanceRollbackMode::Disabled, Importance_Height, holder.get());

		// Assert:
		context.assertNoFiles();
	}

	// endregion

	// region createReadCalculator

	TEST(TEST_CLASS, ReadCalculatorFailsWhenIndexFileIsNotPresent) {
		// Arrange:
		static constexpr auto Importance_Height = model::ImportanceHeight(246);

		TestContext context;
		CacheHolder holder;
		auto addresses = holder.addAccounts({ Amount(2000), Amount(1000), Amount(3000) });

		// Act + Assert:
		auto pCalculator = context.createReadCalculator({
			{ { addresses[0], Importance_Height }, { Importance(111), Amount(222), 14, 400 } },
			{ { addresses[1], Importance_Height }, { Importance(123), Amount(432), 22, 200 } },
			{ { addresses[2], Importance_Height }, { Importance(444), Amount(110), 10, 300 } }
		});
		EXPECT_THROW(pCalculator->recalculate(Default_Calculation_Mode, Importance_Height, holder.get()), catapult_runtime_error);
	}

	TEST(TEST_CLASS, ReadCalculatorFailsWhenRequiredImportanceFileIsNotPresent) {
		// Arrange:
		static constexpr auto Importance_Height = model::ImportanceHeight(246);

		TestContext context;
		context.setIndexValue(492);

		CacheHolder holder;
		auto addresses = holder.addAccounts({ Amount(2000), Amount(1000), Amount(3000) });

		// Act + Assert:
		auto pCalculator = context.createReadCalculator({
			{ { addresses[0], Importance_Height }, { Importance(111), Amount(222), 14, 400 } },
			{ { addresses[1], Importance_Height }, { Importance(123), Amount(432), 22, 200 } },
			{ { addresses[2], Importance_Height }, { Importance(444), Amount(110), 10, 300 } }
		});
		EXPECT_THROW(pCalculator->recalculate(Default_Calculation_Mode, Importance_Height, holder.get()), catapult_runtime_error);
	}

	TEST(TEST_CLASS, ReadCalculatorBypassesFileLoadingWhenIndexValueMatchesNextImportanceHeight) {
		// Arrange:
		static constexpr auto Previous_Importance_Height = model::ImportanceHeight(123);
		static constexpr auto Importance_Height = model::ImportanceHeight(246);

		TestContext context;
		context.setIndexValue(369);

		CacheHolder holder;
		auto addresses = holder.addAccounts({ Amount(2000), Amount(1000), Amount(3000) });

		// Act:
		auto pCalculator = context.createReadCalculator({
			{ { addresses[0], Importance_Height }, { Importance(111), Amount(222), 14, 400 } },
			{ { addresses[1], Importance_Height }, { Importance(123), Amount(432), 22, 200 } },
			{ { addresses[2], Importance_Height }, { Importance(444), Amount(110), 10, 300 } }
		});
		pCalculator->recalculate(Default_Calculation_Mode, Importance_Height, holder.get());

		// Assert: file loading was skipped and importances were set by decorated calculator
		//         (since the mock calculator emulates commit, the expected output here doesn't match what a real restore
		//          calculator would produce)
		holder.assertEqualImportances(addresses[0], 0, { { Importance(111), Importance_Height } });
		holder.assertEqualImportances(addresses[1], 1, { { Importance(123), Importance_Height } });
		holder.assertEqualImportances(addresses[2], 2, { { Importance(444), Importance_Height } });

		holder.assertEqualActivityBuckets(addresses[0], 0, {
			{ { Amount(0), 0, 0 }, Importance_Height },
			{ { Amount(222), 14, 400 }, Previous_Importance_Height }
		});
		holder.assertEqualActivityBuckets(addresses[1], 1, {
			{ { Amount(0), 0, 0 }, Importance_Height },
			{ { Amount(432), 22, 200 }, Previous_Importance_Height }
		});
		holder.assertEqualActivityBuckets(addresses[2], 2, {
			{ { Amount(0), 0, 0 }, Importance_Height },
			{ { Amount(110), 10, 300 }, Previous_Importance_Height }
		});
	}

	// endregion

	// region createReadCalculator (RollbackDisabled)

	TEST(TEST_CLASS, ReadCalculatorFails_RollbackDisabled) {
		// Arrange:
		static constexpr auto Importance_Height = model::ImportanceHeight(246);

		TestContext context;
		context.setIndexValue(369);

		CacheHolder holder;
		auto addresses = holder.addAccounts({ Amount(2000), Amount(1000), Amount(3000) });

		// Act:
		auto pCalculator = context.createReadCalculator({
			{ { addresses[0], Importance_Height }, { Importance(111), Amount(222), 14, 400 } },
			{ { addresses[1], Importance_Height }, { Importance(123), Amount(432), 22, 200 } },
			{ { addresses[2], Importance_Height }, { Importance(444), Amount(110), 10, 300 } }
		});
		EXPECT_THROW(
				pCalculator->recalculate(ImportanceRollbackMode::Disabled, Importance_Height, holder.get()),
				catapult_invalid_argument);
	}

	// endregion

	// region roundtrip

	namespace {
		struct MinimumAccountsTraits {
		public:
			static constexpr auto Should_Seed_Additional_Accounts = false;

		public:
			class CacheHolderFactory {
			public:
				CacheHolderFactory(
						const std::vector<Address>&,
						const std::vector<Amount>&,
						const std::vector<Address>& relevantAddresses,
						const std::vector<Amount>& relevantBalances) {
					m_holder.addAccounts(relevantAddresses, relevantBalances);
				}

			public:
				auto& calculateHolder() {
					return m_holder;
				}

				auto& restoreHolder() {
					return m_holder;
				}

			private:
				CacheHolder m_holder;
			};
		};

		struct AdditionalAccountsTraits {
		public:
			static constexpr auto Should_Seed_Additional_Accounts = true;

		public:
			class CacheHolderFactory {
			public:
				CacheHolderFactory(
						const std::vector<Address>& addresses,
						const std::vector<Amount>& balances,
						const std::vector<Address>& relevantAddresses,
						const std::vector<Amount>& relevantBalances) {
					m_calculateHolder.addAccounts(addresses, balances);
					m_restoreHolder.addAccounts(relevantAddresses, relevantBalances);
				}

			public:
				auto& calculateHolder() {
					return m_calculateHolder;
				}

				auto& restoreHolder() {
					return m_restoreHolder;
				}

			private:
				CacheHolder m_calculateHolder;
				CacheHolder m_restoreHolder;
			};
		};

		template<typename TTraits, typename TAction>
		void PrepareRollbackTest(TAction action) {
			// Arrange:
			TestContext context;

			auto addresses = test::GenerateRandomDataVector<Address>(5);
			auto balances = std::vector<Amount>{ Amount(2000), Amount(1000), Amount(3000), Amount(4000), Amount(7000) };
			auto relevantAddresses = std::vector<Address>{ addresses[0], addresses[2], addresses[4] };
			auto relevantBalances = std::vector<Amount>{ balances[0], balances[2], balances[4] };
			typename TTraits::CacheHolderFactory holderFactory(addresses, balances, relevantAddresses, relevantBalances);

			MockImportanceCalculator::RecalculateMap recalculateMap;
			auto heights = std::initializer_list<uint64_t>{ 1, 123, 246, 369, 492, 615, 738, 861 };
			for (auto height : heights) {
				auto importanceHeight = model::ImportanceHeight(height);
				recalculateMap.insert({ { addresses[0], importanceHeight }, { Importance(111 * height), Amount(222), 14, 400 + height } });

				if (TTraits::Should_Seed_Additional_Accounts)
					recalculateMap.insert({ { addresses[1], importanceHeight }, { Importance(999 * height), Amount(5), 11, 6 + height } });

				recalculateMap.insert({ { addresses[2], importanceHeight }, { Importance(123 * height), Amount(432), 22, 200 + height } });

				if (TTraits::Should_Seed_Additional_Accounts)
					recalculateMap.insert({ { addresses[3], importanceHeight }, { Importance(876 * height), Amount(8), 19, 5 + height } });

				recalculateMap.insert({ { addresses[4], importanceHeight }, { Importance(444 * height), Amount(110), 10, 300 + height } });
			}

			// - recalculate at all heights from nemesis
			auto pCalculator = context.createWriteCalculator(recalculateMap);
			for (auto height : heights)
				pCalculator->recalculate(Default_Calculation_Mode, model::ImportanceHeight(height), holderFactory.calculateHolder().get());

			// -  limit the highValueAccounts to the three relevant accounts
			action(relevantAddresses, context, holderFactory.restoreHolder());
		}

		template<typename TTraits>
		void AssertCanRoundtripToPointAfterNemesis() {
			// Arrange:
			PrepareRollbackTest<TTraits>([](const auto& addresses, const auto& context, auto& holder) {
				static constexpr auto Importance_Height = model::ImportanceHeight(615);

				// Act:
				auto pCalculator = context.createReadCalculator({});
				pCalculator->recalculate(Default_Calculation_Mode, Importance_Height, holder.get());

				// Assert: importance at recalculate point should be restored
				holder.assertEqualImportances(addresses[0], 0, { { Importance(111 * 615), Importance_Height } });
				holder.assertEqualImportances(addresses[1], 1, { { Importance(123 * 615), Importance_Height } });
				holder.assertEqualImportances(addresses[2], 2, { { Importance(444 * 615), Importance_Height } });

				// - activity buckets through recalculate point should be restored (only raw score is modified as sentinel)
				//   important: in order to calculate importances correctly, Activity_Bucket_History_Size buckets need to be restored
				holder.assertEqualActivityBuckets(addresses[0], 0, {
					{ { Amount(222), 14, 400 + 738 }, Importance_Height },
					{ { Amount(222), 14, 400 + 615 }, Importance_Height - model::ImportanceHeight(1 * Importance_Grouping) },
					{ { Amount(222), 14, 400 + 492 }, Importance_Height - model::ImportanceHeight(2 * Importance_Grouping) },
					{ { Amount(222), 14, 400 + 369 }, Importance_Height - model::ImportanceHeight(3 * Importance_Grouping) },
					{ { Amount(222), 14, 400 + 246 }, Importance_Height - model::ImportanceHeight(4 * Importance_Grouping) }
				});
				holder.assertEqualActivityBuckets(addresses[1], 1, {
					{ { Amount(432), 22, 200 + 738 }, Importance_Height },
					{ { Amount(432), 22, 200 + 615 }, Importance_Height - model::ImportanceHeight(1 * Importance_Grouping) },
					{ { Amount(432), 22, 200 + 492 }, Importance_Height - model::ImportanceHeight(2 * Importance_Grouping) },
					{ { Amount(432), 22, 200 + 369 }, Importance_Height - model::ImportanceHeight(3 * Importance_Grouping) },
					{ { Amount(432), 22, 200 + 246 }, Importance_Height - model::ImportanceHeight(4 * Importance_Grouping) }
				});
				holder.assertEqualActivityBuckets(addresses[2], 2, {
					{ { Amount(110), 10, 300 + 738 }, Importance_Height },
					{ { Amount(110), 10, 300 + 615 }, Importance_Height - model::ImportanceHeight(1 * Importance_Grouping) },
					{ { Amount(110), 10, 300 + 492 }, Importance_Height - model::ImportanceHeight(2 * Importance_Grouping) },
					{ { Amount(110), 10, 300 + 369 }, Importance_Height - model::ImportanceHeight(3 * Importance_Grouping) },
					{ { Amount(110), 10, 300 + 246 }, Importance_Height - model::ImportanceHeight(4 * Importance_Grouping) }
				});
			});
		}

		template<typename TTraits>
		void AssertCanRoundtripToNemesis() {
			// Arrange:
			PrepareRollbackTest<TTraits>([](const auto& addresses, const auto& context, auto& holder) {
				static constexpr auto Importance_Height = model::ImportanceHeight(1);

				// Act:
				auto pCalculator = context.createReadCalculator({});
				pCalculator->recalculate(Default_Calculation_Mode, Importance_Height, holder.get());

				// Assert: importance at recalculate point should be restored
				holder.assertEqualImportances(addresses[0], 0, { { Importance(111 * 1), Importance_Height } });
				holder.assertEqualImportances(addresses[1], 1, { { Importance(123 * 1), Importance_Height } });
				holder.assertEqualImportances(addresses[2], 2, { { Importance(444 * 1), Importance_Height } });

				// - activity buckets through recalculate point should be restored (only raw score is modified as sentinel)
				holder.assertEqualActivityBuckets(addresses[0], 0, { { { Amount(222), 14, 400 + 123 }, Importance_Height } });
				holder.assertEqualActivityBuckets(addresses[1], 1, { { { Amount(432), 22, 200 + 123 }, Importance_Height } });
				holder.assertEqualActivityBuckets(addresses[2], 2, { { { Amount(110), 10, 300 + 123 }, Importance_Height } });
			});
		}
	}

	TEST(TEST_CLASS, CanRoundtripToPointAfterNemesis) {
		AssertCanRoundtripToPointAfterNemesis<MinimumAccountsTraits>();
	}

	TEST(TEST_CLASS, CanRoundtripToPointAfterNemesisWithIgnoredAccountsInImporanceFiles) {
		AssertCanRoundtripToPointAfterNemesis<AdditionalAccountsTraits>();
	}

	TEST(TEST_CLASS, CanRoundtripToNemesis) {
		AssertCanRoundtripToNemesis<MinimumAccountsTraits>();
	}

	TEST(TEST_CLASS, CanRoundtripToNemesisWithIgnoredAccountsInImporanceFiles) {
		AssertCanRoundtripToNemesis<AdditionalAccountsTraits>();
	}

	// endregion
}}
