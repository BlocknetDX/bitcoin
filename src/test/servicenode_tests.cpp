// Copyright (c) 2019 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/staking_tests.h>

#include <node/transaction.h>
#include <rpc/server.h>
#include <servicenode/servicenode.h>
#include <servicenode/servicenodemgr.h>
#include <wallet/coincontrol.h>

sn::ServiceNode snodeNetwork(const CPubKey & snodePubKey, const uint8_t & tier, const CKeyID & paymentAddr,
                         const std::vector<COutPoint> & collateral, const uint32_t & blockNumber,
                         const uint256 & blockHash, const std::vector<unsigned char> & sig)
{
    auto ss = CDataStream(SER_NETWORK, PROTOCOL_VERSION);
    ss << snodePubKey << tier << paymentAddr << collateral << blockNumber << blockHash << sig;
    sn::ServiceNode snode; ss >> snode;
    return snode;
}

/**
 * Save configuration files to the specified path.
 */
void saveFile(const boost::filesystem::path& p, const std::string& str) {
    boost::filesystem::ofstream file;
    file.exceptions(std::ofstream::failbit | std::ofstream::badbit);
    file.open(p, std::ios_base::binary);
    file.write(str.c_str(), str.size());
}

BOOST_AUTO_TEST_SUITE(servicenode_tests)

/// Check case where servicenode is properly validated under normal circumstances
BOOST_AUTO_TEST_CASE(servicenode_tests_isvalid)
{
    TestChainPoS pos(false);
    auto *params = (CChainParams*)&Params();
    params->consensus.GetBlockSubsidy = [](const int & blockHeight, const Consensus::Params & consensusParams) {
        if (blockHeight <= consensusParams.lastPOWBlock)
            return 1000 * COIN;
        return 1 * COIN;
    };
    pos.Init();

    const auto snodePubKey = pos.coinbaseKey.GetPubKey();
    const auto tier = sn::ServiceNode::Tier::SPV;

    CAmount totalAmount{0};
    std::vector<COutPoint> collateral;
    for (const auto & tx : pos.m_coinbase_txns) {
        if (!GetTxFunc({tx->GetHash(), 0})) // make sure tx exists
            continue;
        totalAmount += tx->vout[0].nValue;
        collateral.emplace_back(tx->GetHash(), 0);
        if (totalAmount >= sn::ServiceNode::COLLATERAL_SPV)
            break;
    }

    // Generate the signature from sig hash
    const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash());
    std::vector<unsigned char> sig;
    BOOST_CHECK(pos.coinbaseKey.SignCompact(sighash, sig));

    // Deserialize servicenode obj from network stream
    sn::ServiceNode snode;
    BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash(), sig));
    BOOST_CHECK(snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc));
}

/// Check open tier case
BOOST_FIXTURE_TEST_CASE(servicenode_tests_opentier, TestChainPoS)
{
    CKey key; key.MakeNewKey(true);
    const auto snodePubKey = key.GetPubKey();
    const auto tier = sn::ServiceNode::Tier::OPEN;
    const auto collateral = std::vector<COutPoint>();

    // Valid check
    {
        // Generate the signature from sig hash
        const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash());
        std::vector<unsigned char> sig;
        BOOST_CHECK(key.SignCompact(sighash, sig));
        // Deserialize servicenode obj from network stream
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash(), sig));
        BOOST_CHECK_MESSAGE(snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc), "Failed on valid snode key sig");
    }

    // Case where wrong key is used to generate sig. For the open tier the snode private key
    // must be used to generate the signature. In this test we use another key.
    {
        // Generate the signature from sig hash
        const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash());
        std::vector<unsigned char> sig;
        BOOST_CHECK(coinbaseKey.SignCompact(sighash, sig));  // use invalid coinbase key (invalid for open tier)
        // Deserialize servicenode obj from network stream
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash(), sig));
        BOOST_CHECK_MESSAGE(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc), "Failed on invalid snode key sig");
    }
}

/// Check case where duplicate collateral utxos are used
BOOST_FIXTURE_TEST_CASE(servicenode_tests_duplicate_collateral, TestChainPoS)
{
    CKey key; key.MakeNewKey(true);
    const auto snodePubKey = key.GetPubKey();
    const auto tier = sn::ServiceNode::Tier::SPV;

    // Assumes total input amounts below adds up to ServiceNode::COLLATERAL_SPV
    CAmount totalAmount{0};
    std::vector<COutPoint> collateral;
    while (totalAmount < sn::ServiceNode::COLLATERAL_SPV) {
        collateral.emplace_back(m_coinbase_txns[0]->GetHash(), 0);
        totalAmount += m_coinbase_txns[0]->GetValueOut();
    }

    // Generate the signature from sig hash
    const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash());
    std::vector<unsigned char> sig;
    BOOST_CHECK(coinbaseKey.SignCompact(sighash, sig));

    // Deserialize servicenode obj from network stream
    sn::ServiceNode snode;
    BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash(), sig));
    BOOST_CHECK(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc));
}

/// Check case where there's not enough snode inputs
BOOST_FIXTURE_TEST_CASE(servicenode_tests_insufficient_collateral, TestChainPoS)
{
    CKey key; key.MakeNewKey(true);
    const auto snodePubKey = key.GetPubKey();
    const auto tier = sn::ServiceNode::Tier::SPV;

    // Assumes total input amounts below adds up to ServiceNode::COLLATERAL_SPV
    std::vector<COutPoint> collateral;
    collateral.emplace_back(m_coinbase_txns[0]->GetHash(), 0);
    BOOST_CHECK(m_coinbase_txns[0]->GetValueOut() < sn::ServiceNode::COLLATERAL_SPV);

    // Generate the signature from sig hash
    const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash());
    std::vector<unsigned char> sig;
    BOOST_CHECK(coinbaseKey.SignCompact(sighash, sig));

    // Deserialize servicenode obj from network stream
    sn::ServiceNode snode;
    BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash(), sig));
    BOOST_CHECK(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc));
}

/// Check case where collateral inputs are spent
BOOST_AUTO_TEST_CASE(servicenode_tests_spent_collateral)
{
    TestChainPoS pos(false);
    auto *params = (CChainParams*)&Params();
    params->consensus.GetBlockSubsidy = [](const int & blockHeight, const Consensus::Params & consensusParams) {
        if (blockHeight <= consensusParams.lastPOWBlock)
            return 1000 * COIN;
        return 1 * COIN;
    };
    pos.Init();

    CKey key; key.MakeNewKey(true);
    const auto snodePubKey = key.GetPubKey();
    const auto tier = sn::ServiceNode::Tier::SPV;

    CBasicKeyStore keystore; // temp used to spend inputs
    keystore.AddKey(pos.coinbaseKey);

    // Spend inputs that would be used in snode collateral
    {
        std::vector<COutput> coins;
        {
            LOCK2(cs_main, pos.wallet->cs_wallet);
            pos.wallet->AvailableCoins(*pos.locked_chain, coins);
        }
        // Spend the first available input in "coins"
        auto c = coins[0];
        CMutableTransaction mtx;
        mtx.vin.resize(1);
        mtx.vin[0] = CTxIn(c.GetInputCoin().outpoint);
        mtx.vout.resize(1);
        mtx.vout[0].scriptPubKey = GetScriptForRawPubKey(snodePubKey);
        mtx.vout[0].nValue = c.GetInputCoin().txout.nValue - CENT;
        SignatureData sigdata = DataFromTransaction(mtx, 0, c.GetInputCoin().txout);
        ProduceSignature(keystore, MutableTransactionSignatureCreator(&mtx, 0, mtx.vout[0].nValue, SIGHASH_ALL), c.GetInputCoin().txout.scriptPubKey, sigdata);
        UpdateInput(mtx.vin[0], sigdata);
        // Send transaction
        uint256 txid; std::string errstr;
        const TransactionError err = BroadcastTransaction(MakeTransactionRef(mtx), txid, errstr, 0);
        BOOST_CHECK_MESSAGE(err == TransactionError::OK, strprintf("Failed to send snode collateral spent tx: %s", errstr));
        pos.StakeBlocks(1), SyncWithValidationInterfaceQueue();
        CBlock block;
        BOOST_CHECK(ReadBlockFromDisk(block, chainActive.Tip(), params->GetConsensus()));
        BOOST_CHECK_MESSAGE(block.vtx.size() >= 3 && block.vtx[2]->GetHash() == mtx.GetHash(), "Expected transaction to be included in latest block");
        Coin cn;
        BOOST_CHECK_MESSAGE(!pcoinsTip->GetCoin(c.GetInputCoin().outpoint, cn), "Coin should be spent here");

        CAmount totalAmount{0};
        std::vector<COutPoint> collateral;
        for (int i = 0; i < coins.size(); ++i) {
            const auto & coin = coins[i];
            const auto txn = coin.tx->tx;
            totalAmount += txn->GetValueOut();
            collateral.emplace_back(txn->GetHash(), 0);
            if (totalAmount >= sn::ServiceNode::COLLATERAL_SPV)
                break;
        }

        // Generate the signature from sig hash
        const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash());
        std::vector<unsigned char> sig;
        BOOST_CHECK(pos.coinbaseKey.SignCompact(sighash, sig));
        // Deserialize servicenode obj from network stream
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash(), sig));
        BOOST_CHECK_MESSAGE(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc), "Should fail on spent collateral");
    }

    // Check case where spent collateral is in mempool
    {
        std::vector<COutput> coins;
        {
            LOCK2(cs_main, pos.wallet->cs_wallet);
            pos.wallet->AvailableCoins(*pos.locked_chain, coins);
        }
        // Spend one of the collateral inputs (spend the 2nd coinbase input, b/c first was spent above)
        COutput c = coins[0];
        CMutableTransaction mtx;
        mtx.vin.resize(1);
        mtx.vin[0] = CTxIn(c.GetInputCoin().outpoint);
        mtx.vout.resize(1);
        mtx.vout[0].scriptPubKey = GetScriptForRawPubKey(snodePubKey);
        mtx.vout[0].nValue = c.GetInputCoin().txout.nValue - CENT;
        SignatureData sigdata = DataFromTransaction(mtx, 0, c.GetInputCoin().txout);
        ProduceSignature(keystore, MutableTransactionSignatureCreator(&mtx, 0, mtx.vout[0].nValue, SIGHASH_ALL), c.GetInputCoin().txout.scriptPubKey, sigdata);
        UpdateInput(mtx.vin[0], sigdata);

        CValidationState state;
        LOCK(cs_main);
        BOOST_CHECK(AcceptToMemoryPool(mempool, state, MakeTransactionRef(mtx),
                                                    nullptr, // pfMissingInputs
                                                    nullptr, // plTxnReplaced
                                                    false,   // bypass_limits
                                                    0));     // nAbsurdFee
        CAmount totalAmount{0};
        std::vector<COutPoint> collateral;
        for (int i = 1; i < coins.size(); ++i) { // start at 1 (ignore first spent coinbase)
            const auto & coin = coins[i];
            const auto txn = coin.tx->tx;
            totalAmount += txn->GetValueOut();
            collateral.emplace_back(txn->GetHash(), 0);
            if (totalAmount >= sn::ServiceNode::COLLATERAL_SPV)
                break;
        }

        // Generate the signature from sig hash
        const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash());
        std::vector<unsigned char> sig;
        BOOST_CHECK(pos.coinbaseKey.SignCompact(sighash, sig));
        // Deserialize servicenode obj from network stream
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash(), sig));
        BOOST_CHECK_MESSAGE(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc), "Should fail on spent collateral in mempool");
    }
}

/// Servicenode registration and ping tests
BOOST_AUTO_TEST_CASE(servicenode_tests_registration_pings)
{
    TestChainPoS pos(false);
    auto *params = (CChainParams*)&Params();
    params->consensus.GetBlockSubsidy = [](const int & blockHeight, const Consensus::Params & consensusParams) {
        if (blockHeight <= consensusParams.lastPOWBlock)
            return 1000 * COIN;
        return 1 * COIN;
    };
    pos.Init();

    CTxDestination dest(pos.coinbaseKey.GetPubKey().GetID());
    int addedSnodes{0};

    // Snode registration and ping w/ uncompressed key
    {
        CKey key; key.MakeNewKey(false);
        BOOST_CHECK_MESSAGE(sn::ServiceNodeMgr::instance().registerSn(key, sn::ServiceNode::SPV, EncodeDestination(dest), g_connman.get(), {pos.wallet}), "Register snode w/ uncompressed key");
        // Snode ping w/ uncompressed key
        sn::ServiceNodeConfigEntry entry("snode0", sn::ServiceNode::SPV, key, dest);
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>{entry});
        std::set<sn::ServiceNodeConfigEntry> entries;
        sn::ServiceNodeMgr::instance().loadSnConfig(entries);
        BOOST_CHECK_MESSAGE(sn::ServiceNodeMgr::instance().sendPing(50, "BLOCK,BTC,LTC", g_connman.get()), "Snode ping w/ uncompressed key");
        ++addedSnodes;
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
    }

    // Snode registration and ping w/ compressed key
    {
        CKey key; key.MakeNewKey(true);
        BOOST_CHECK_MESSAGE(sn::ServiceNodeMgr::instance().registerSn(key, sn::ServiceNode::SPV, EncodeDestination(dest), g_connman.get(), {pos.wallet}), "Register snode w/ compressed key");
        // Snode ping w/ compressed key
        sn::ServiceNodeConfigEntry entry("snode1", sn::ServiceNode::SPV, key, dest);
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>{entry});
        std::set<sn::ServiceNodeConfigEntry> entries;
        sn::ServiceNodeMgr::instance().loadSnConfig(entries);
        BOOST_CHECK_MESSAGE(sn::ServiceNodeMgr::instance().sendPing(50, "BLOCK,BTC,LTC", g_connman.get()), "Snode ping w/ compressed key");
        ++addedSnodes;
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
    }

    // Check snode count matches number added above
    BOOST_CHECK(sn::ServiceNodeMgr::instance().list().size() == addedSnodes);
    sn::ServiceNodeMgr::instance().reset();

    // Check servicenoderegister all rpc
    {
        const auto & saddr = EncodeDestination(GetDestinationForKey(pos.coinbaseKey.GetPubKey(), OutputType::LEGACY));
        UniValue rpcparams(UniValue::VARR);
        rpcparams.push_backV({ "auto", 2, saddr });
        UniValue entries;
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesetup", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 2, "Service node config count should match expected");
        rpcparams = UniValue(UniValue::VARR);
        BOOST_CHECK_NO_THROW(CallRPC2("servicenoderegister", rpcparams));
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Check servicenoderegister by alias rpc
    {
        const auto & saddr = EncodeDestination(GetDestinationForKey(pos.coinbaseKey.GetPubKey(), OutputType::LEGACY));
        UniValue rpcparams(UniValue::VARR);
        rpcparams.push_backV({ "auto", 2, saddr });
        UniValue entries;
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesetup", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 2, "Service node config count should match expected");
        rpcparams = UniValue(UniValue::VARR);
        rpcparams.push_backV({ "snode1" });
        BOOST_CHECK_NO_THROW(CallRPC2("servicenoderegister", rpcparams));
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Check servicenoderegister rpc result data
    {
        const auto & saddr = EncodeDestination(GetDestinationForKey(pos.coinbaseKey.GetPubKey(), OutputType::LEGACY));
        UniValue rpcparams(UniValue::VARR);
        rpcparams.push_backV({ "auto", 2, saddr });
        UniValue entries;
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesetup", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 2, "Service node config count should match expected");
        rpcparams = UniValue(UniValue::VARR);
        try {
            auto result = CallRPC2("servicenoderegister", rpcparams);
            BOOST_CHECK_EQUAL(result.isArray(), true);
            UniValue o = result[1];
            BOOST_CHECK_EQUAL(find_value(o, "alias").get_str(), "snode1");
            BOOST_CHECK_EQUAL(find_value(o, "tier").get_str(), sn::ServiceNodeMgr::tierString(sn::ServiceNode::SPV));
            BOOST_CHECK_EQUAL(find_value(o, "snodekey").get_str().empty(), false); // check not empty
            BOOST_CHECK_EQUAL(find_value(o, "address").get_str(), saddr);
        } catch (std::exception & e) {
            BOOST_CHECK_MESSAGE(false, strprintf("servicenoderegister failed: %s", e.what()));
        }

        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Check servicenoderegister bad alias
    {
        const auto & saddr = EncodeDestination(GetDestinationForKey(pos.coinbaseKey.GetPubKey(), OutputType::LEGACY));
        UniValue rpcparams(UniValue::VARR);
        rpcparams.push_backV({ "auto", 2, saddr });
        UniValue entries;
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesetup", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 2, "Service node config count should match expected");
        rpcparams = UniValue(UniValue::VARR);
        rpcparams.push_backV({ "bad_alias" });
        BOOST_CHECK_THROW(CallRPC2("servicenoderegister", rpcparams), std::runtime_error);
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Check servicenoderegister no configs
    {
        const auto & saddr = EncodeDestination(GetDestinationForKey(pos.coinbaseKey.GetPubKey(), OutputType::LEGACY));
        UniValue rpcparams(UniValue::VARR);
        BOOST_CHECK_THROW(CallRPC2("servicenoderegister", rpcparams), std::runtime_error);
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
    sn::ServiceNodeMgr::instance().reset();
}

/// Check misc cases
BOOST_AUTO_TEST_CASE(servicenode_tests_misc_checks)
{
    TestChainPoS pos(false);
    auto *params = (CChainParams*)&Params();
    params->consensus.GetBlockSubsidy = [](const int & blockHeight, const Consensus::Params & consensusParams) {
        if (blockHeight <= consensusParams.lastPOWBlock)
            return 1000 * COIN;
        return 1 * COIN;
    };
    pos.Init();

    auto & smgr = sn::ServiceNodeMgr::instance();
    CKey key; key.MakeNewKey(true);
    const auto snodePubKey = key.GetPubKey();

    std::vector<COutput> coins;
    {
        LOCK2(cs_main, pos.wallet->cs_wallet);
        pos.wallet->AvailableCoins(*pos.locked_chain, coins);
    }
    // sort largest coins first
    std::sort(coins.begin(), coins.end(), [](COutput & a, COutput & b) {
        return a.GetInputCoin().txout.nValue > b.GetInputCoin().txout.nValue;
    });
    CAmount totalAmount{0};
    std::vector<COutPoint> collateral;
    for (const auto & coin : coins) {
        totalAmount += coin.GetInputCoin().txout.nValue;
        collateral.push_back(coin.GetInputCoin().outpoint);
        if (totalAmount >= sn::ServiceNode::COLLATERAL_SPV)
            break;
    }

    // NOTE** This test must be first!
    BOOST_CHECK_MESSAGE(sn::ServiceNodeMgr::instance().list().empty(), "Fail on non-empty snode list");

    // Fail on bad tier
    {
        const uint8_t tier = 0xff;
        // Generate the signature from sig hash
        CHashWriter ss(SER_GETHASH, 0);
        ss << snodePubKey << tier << collateral;
        const auto & sighash = ss.GetHash();
        std::vector<unsigned char> sig;
        BOOST_CHECK(pos.coinbaseKey.SignCompact(sighash, sig));
        // Deserialize servicenode obj from network stream
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash(), sig));
        BOOST_CHECK_MESSAGE(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc), "Fail on bad tier");
    }

    // Fail on empty collateral
    {
        const uint8_t tier = sn::ServiceNode::Tier::SPV;
        std::vector<COutPoint> collateral2;
        // Generate the signature from sig hash
        CHashWriter ss(SER_GETHASH, 0);
        ss << snodePubKey << tier << collateral2;
        const auto & sighash = ss.GetHash();
        std::vector<unsigned char> sig;
        BOOST_CHECK(pos.coinbaseKey.SignCompact(sighash, sig));
        // Deserialize servicenode obj from network stream
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral2, chainActive.Height(), chainActive.Tip()->GetBlockHash(), sig));
        BOOST_CHECK_MESSAGE(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc), "Fail on empty collateral");
    }

    // Fail on empty snode pubkey
    {
        const auto tier = sn::ServiceNode::Tier::SPV;
        const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash());
        std::vector<unsigned char> sig;
        BOOST_CHECK(pos.coinbaseKey.SignCompact(sighash, sig));
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(CPubKey(), tier, CPubKey().GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash(), sig));
        BOOST_CHECK_MESSAGE(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc), "Fail on empty snode pubkey");
    }

    // Fail on empty sighash
    {
        const auto tier = sn::ServiceNode::Tier::SPV;
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height(), chainActive.Tip()->GetBlockHash(), std::vector<unsigned char>()));
        BOOST_CHECK_MESSAGE(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc), "Fail on empty sighash");
    }

    // Fail on bad best block
    {
        const auto tier = sn::ServiceNode::Tier::SPV;
        const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, 0, uint256());
        std::vector<unsigned char> sig;
        BOOST_CHECK(pos.coinbaseKey.SignCompact(sighash, sig));
        // Deserialize servicenode obj from network stream
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, 0, uint256(), sig));
        BOOST_CHECK_MESSAGE(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc), "Fail on bad best block");
    }

    // Fail on stale best block (valid but stale block number)
    {
        const int staleBlockNumber = chainActive.Height()-SNODE_STALE_BLOCKS - 1;
        const auto tier = sn::ServiceNode::Tier::SPV;
        const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, staleBlockNumber, chainActive[staleBlockNumber]->GetBlockHash());
        std::vector<unsigned char> sig;
        BOOST_CHECK(pos.coinbaseKey.SignCompact(sighash, sig));
        // Deserialize servicenode obj from network stream
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, staleBlockNumber, chainActive[staleBlockNumber]->GetBlockHash(), sig));
        BOOST_CHECK_MESSAGE(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc), "Fail on stale best block");
    }

    // Fail on best block number being too far into future
    {
        const auto tier = sn::ServiceNode::Tier::SPV;
        const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height()+5, chainActive[5]->GetBlockHash());
        std::vector<unsigned char> sig;
        BOOST_CHECK(pos.coinbaseKey.SignCompact(sighash, sig));
        // Deserialize servicenode obj from network stream
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, chainActive.Height()+5, chainActive[5]->GetBlockHash(), sig));
        BOOST_CHECK_MESSAGE(!snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc), "Fail on best block, unknown block, too far in future");
    }

    // Test disabling the stale check on the servicenode validation
    {
        const int staleBlockNumber = chainActive.Height()-SNODE_STALE_BLOCKS - 1;
        const auto tier = sn::ServiceNode::Tier::SPV;
        const auto & sighash = sn::ServiceNode::CreateSigHash(snodePubKey, tier, snodePubKey.GetID(), collateral, staleBlockNumber, chainActive[staleBlockNumber]->GetBlockHash());
        std::vector<unsigned char> sig;
        BOOST_CHECK(pos.coinbaseKey.SignCompact(sighash, sig));
        // Deserialize servicenode obj from network stream
        sn::ServiceNode snode;
        BOOST_CHECK_NO_THROW(snode = snodeNetwork(snodePubKey, tier, snodePubKey.GetID(), collateral, staleBlockNumber, chainActive[staleBlockNumber]->GetBlockHash(), sig));
        BOOST_CHECK_MESSAGE(snode.isValid(GetTxFunc, IsServiceNodeBlockValidFunc, false), "Fail on disabled stale check");
    }

    // Test case where snode config doesn't exist on disk
    {
        boost::filesystem::remove(sn::ServiceNodeMgr::getServiceNodeConf());
        std::set<sn::ServiceNodeConfigEntry> entries;
        BOOST_CHECK_MESSAGE(smgr.loadSnConfig(entries), "Should load default config");
        BOOST_CHECK_MESSAGE(entries.empty(), "Snode configs should match expected size");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test snode config for OPEN tier
    {
        const auto & skey = EncodeSecret(key);
        saveFile(sn::ServiceNodeMgr::getServiceNodeConf(), "mn1 OPEN " + skey);
        std::set<sn::ServiceNodeConfigEntry> entries;
        BOOST_CHECK_MESSAGE(smgr.loadSnConfig(entries), "Should load OPEN tier config");
        BOOST_CHECK_MESSAGE(entries.size() == 1, "OPEN tier config should match expected size");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test snode config for SPV tier
    {
        const auto & skey = EncodeSecret(key);
        const auto & saddr = EncodeDestination(GetDestinationForKey(key.GetPubKey(), OutputType::LEGACY));
        saveFile(sn::ServiceNodeMgr::getServiceNodeConf(), "mn1 SPV " + skey + " " + saddr);
        std::set<sn::ServiceNodeConfigEntry> entries;
        BOOST_CHECK_MESSAGE(smgr.loadSnConfig(entries), "Should load SPV tier config");
        BOOST_CHECK_MESSAGE(entries.size() == 1, "SPV tier config should match expected size");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test snode config for multiple tiers
    {
        const auto & skey = EncodeSecret(key);
        CKey key2; key2.MakeNewKey(true);
        const auto & skey2 = EncodeSecret(key2);
        const auto & saddr2 = EncodeDestination(GetDestinationForKey(key2.GetPubKey(), OutputType::LEGACY));
        saveFile(sn::ServiceNodeMgr::getServiceNodeConf(), "mn1 OPEN " + skey + "\n"
                                                           "mn2 SPV " + skey2 + " " + saddr2);
        std::set<sn::ServiceNodeConfigEntry> entries;
        BOOST_CHECK_MESSAGE(smgr.loadSnConfig(entries), "Should load multi-entry config");
        BOOST_CHECK_MESSAGE(entries.size() == 2, "Multi-entry config should match expected size");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test lowercase tiers
    {
        const auto & skey = EncodeSecret(key);
        CKey key2; key2.MakeNewKey(true);
        const auto & skey2 = EncodeSecret(key2);
        const auto & saddr2 = EncodeDestination(GetDestinationForKey(key2.GetPubKey(), OutputType::LEGACY));
        saveFile(sn::ServiceNodeMgr::getServiceNodeConf(), "mn1 open " + skey + "\n"
                                                           "mn2 spv " + skey2 + " " + saddr2);
        std::set<sn::ServiceNodeConfigEntry> entries;
        BOOST_CHECK_MESSAGE(smgr.loadSnConfig(entries), "Should load lowercase tiers");
        BOOST_CHECK_MESSAGE(entries.size() == 2, "Lowercase tiers config should match expected size");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test bad snode configs
    {
        const auto & skey = EncodeSecret(key);
        CKey key2; key2.MakeNewKey(true);
        const auto & skey2 = EncodeSecret(key2);
        const auto & saddr2 = EncodeDestination(GetDestinationForKey(key2.GetPubKey(), OutputType::LEGACY));

        // Test bad tiers
        saveFile(sn::ServiceNodeMgr::getServiceNodeConf(), "mn1 CUSTOM " + skey + "\n"
                                                           "mn2 SPVV " + skey2 + " " + saddr2);
        std::set<sn::ServiceNodeConfigEntry> entries;
        BOOST_CHECK_MESSAGE(smgr.loadSnConfig(entries), "Should not load bad tiers");
        BOOST_CHECK_MESSAGE(entries.empty(), "Bad tiers config should match expected size");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();

        // Test bad keys
        saveFile(sn::ServiceNodeMgr::getServiceNodeConf(), "mn1 OPEN fkjdsakfjdsakfjksadjfkasjk\n"
                                                           "mn2 SPV djfksadjfkdasjkfajsk " + saddr2);
        BOOST_CHECK_MESSAGE(smgr.loadSnConfig(entries), "Should not load bad keys");
        BOOST_CHECK_MESSAGE(entries.empty(), "Bad keys config should match expected size");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();

        // Test bad address
        saveFile(sn::ServiceNodeMgr::getServiceNodeConf(), "mn1 OPEN " + skey + " jdfksjkfajsdkfjaksdfjaksdjk\n"
                                                           "mn2 SPV " + skey2 + " dsjfksdjkfdsjkfdsjkfjskdjfksdsjk");
        BOOST_CHECK_MESSAGE(smgr.loadSnConfig(entries), "Should not load bad addresses");
        BOOST_CHECK_MESSAGE(entries.empty(), "Bad addresses config should match expected size");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test optional address on OPEN tier
    {
        const auto & skey = EncodeSecret(key);
        const auto & saddr = EncodeDestination(GetDestinationForKey(key.GetPubKey(), OutputType::LEGACY));
        saveFile(sn::ServiceNodeMgr::getServiceNodeConf(), "mn1 OPEN " + skey + " " + saddr);
        std::set<sn::ServiceNodeConfigEntry> entries;
        BOOST_CHECK_MESSAGE(smgr.loadSnConfig(entries), "Should load optional address");
        BOOST_CHECK_MESSAGE(entries.size() == 1, "Optional address config should match expected size");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test missing address on SPV tier
    {
        const auto & skey = EncodeSecret(key);
        const auto & saddr = EncodeDestination(GetDestinationForKey(key.GetPubKey(), OutputType::LEGACY));
        saveFile(sn::ServiceNodeMgr::getServiceNodeConf(), "mn1 SPV " + skey);
        std::set<sn::ServiceNodeConfigEntry> entries;
        BOOST_CHECK_MESSAGE(smgr.loadSnConfig(entries), "Should not load missing address");
        BOOST_CHECK_MESSAGE(entries.empty(), "Missing address config should match expected size");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }
}

/// Check rpc cases
BOOST_AUTO_TEST_CASE(servicenode_tests_rpc)
{
    TestChainPoS pos(false);
    auto *params = (CChainParams*)&Params();
    params->consensus.GetBlockSubsidy = [](const int & blockHeight, const Consensus::Params & consensusParams) {
        if (blockHeight <= consensusParams.lastPOWBlock)
            return 1250 * COIN;
        return 50 * COIN;
    };
    pos.Init();

    auto & smgr = sn::ServiceNodeMgr::instance();
    const auto snodePubKey = pos.coinbaseKey.GetPubKey();
    const auto & saddr = EncodeDestination(GetDestinationForKey(snodePubKey, OutputType::LEGACY));

    CAmount totalAmount{0};
    std::vector<COutPoint> collateral;
    for (const auto & tx : pos.m_coinbase_txns) {
        if (totalAmount >= sn::ServiceNode::COLLATERAL_SPV)
            break;
        totalAmount += tx->GetValueOut();
        collateral.emplace_back(tx->GetHash(), 0);
    }

    // Test rpc servicenode setup
    {
        UniValue rpcparams(UniValue::VARR);
        rpcparams.push_backV({ "auto", 1, saddr });
        UniValue entries;
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesetup", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 1, "Service node config count should match");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
        rpcparams = UniValue(UniValue::VARR);
        rpcparams.push_backV({ "auto", 10, saddr });
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesetup", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 10, "Service node config count should match");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test servicenode.conf formatting
    {
        UniValue rpcparams(UniValue::VARR);
        rpcparams.push_backV({ "auto", 10, saddr });
        BOOST_CHECK_NO_THROW(CallRPC2("servicenodesetup", rpcparams));
        std::set<sn::ServiceNodeConfigEntry> entries;
        BOOST_CHECK_MESSAGE(smgr.loadSnConfig(entries), "Should load config");
        BOOST_CHECK_MESSAGE(entries.size() == 10, "Should load exactly 10 snode config entries");
        // Check servicenode.conf formatting
        for (const auto & entry : entries) {
            const auto & sentry = sn::ServiceNodeMgr::configEntryToString(entry);
            BOOST_CHECK_EQUAL(sentry, strprintf("%s %s %s %s", entry.alias, "SPV", EncodeSecret(entry.key),
                                                EncodeDestination(entry.address)));
        }
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test the servicenodesetup list option
    {
        UniValue rpcparams(UniValue::VARR);
        UniValue list(UniValue::VARR);
        UniValue snode1(UniValue::VOBJ); snode1.pushKV("alias", "snode1"), snode1.pushKV("tier", "SPV"), snode1.pushKV("address", saddr);
        UniValue snode2(UniValue::VOBJ); snode2.pushKV("alias", "snode2"), snode2.pushKV("tier", "SPV"), snode2.pushKV("address", saddr);
        list.push_back(snode1), list.push_back(snode2);
        rpcparams.push_backV({ "list", list });
        UniValue entries;
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesetup", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 2, "Service node config count on list option should match");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test servicenodesetup list option data checks
    {
        UniValue rpcparams;
        UniValue list;
        UniValue snode1 = UniValue(UniValue::VOBJ); snode1.pushKV("alias", "snode1"), snode1.pushKV("tier", "SPV"), snode1.pushKV("address", saddr);
        UniValue snode2;

        // Should fail on missing alias
        rpcparams = UniValue(UniValue::VARR);
        list = UniValue(UniValue::VARR);
        snode2 = UniValue(UniValue::VOBJ); snode2.pushKV("tier", "SPV"), snode2.pushKV("address", saddr);
        list.push_back(snode1), list.push_back(snode2);
        rpcparams.push_backV({ "list", list });
        BOOST_CHECK_THROW(CallRPC2("servicenodesetup", rpcparams), std::runtime_error);

        // Should fail if spaces in alias
        rpcparams = UniValue(UniValue::VARR);
        list = UniValue(UniValue::VARR);
        snode2 = UniValue(UniValue::VOBJ); snode2.pushKV("alias", "snode 2"), snode2.pushKV("tier", "SPV"), snode2.pushKV("address", saddr);
        list.push_back(snode1), list.push_back(snode2);
        rpcparams.push_backV({ "list", list });
        BOOST_CHECK_THROW(CallRPC2("servicenodesetup", rpcparams), std::runtime_error);

        // Should fail on missing tier
        rpcparams = UniValue(UniValue::VARR);
        list = UniValue(UniValue::VARR);
        snode2 = UniValue(UniValue::VOBJ); snode2.pushKV("alias", "snode2"), snode2.pushKV("address", saddr);
        list.push_back(snode1), list.push_back(snode2);
        rpcparams.push_backV({ "list", list });
        BOOST_CHECK_THROW(CallRPC2("servicenodesetup", rpcparams), std::runtime_error);

        // Should fail on bad tier
        rpcparams = UniValue(UniValue::VARR);
        list = UniValue(UniValue::VARR);
        snode2 = UniValue(UniValue::VOBJ); snode2.pushKV("alias", "snode2"), snode2.pushKV("tier", "BAD"), snode2.pushKV("address", saddr);
        list.push_back(snode1), list.push_back(snode2);
        rpcparams.push_backV({ "list", list });
        BOOST_CHECK_THROW(CallRPC2("servicenodesetup", rpcparams), std::runtime_error);

        // Should fail on missing address in non-free tier
        rpcparams = UniValue(UniValue::VARR);
        list = UniValue(UniValue::VARR);
        snode2 = UniValue(UniValue::VOBJ); snode2.pushKV("alias", "snode2"), snode2.pushKV("tier", "SPV");
        list.push_back(snode1), list.push_back(snode2);
        rpcparams.push_backV({ "list", list });
        BOOST_CHECK_THROW(CallRPC2("servicenodesetup", rpcparams), std::runtime_error);

        // Should fail on empty address in non-free tier
        rpcparams = UniValue(UniValue::VARR);
        list = UniValue(UniValue::VARR);
        snode2 = UniValue(UniValue::VOBJ); snode2.pushKV("alias", "snode2"), snode2.pushKV("tier", "SPV"), snode2.pushKV("address", "");
        list.push_back(snode1), list.push_back(snode2);
        rpcparams.push_backV({ "list", list });
        BOOST_CHECK_THROW(CallRPC2("servicenodesetup", rpcparams), std::runtime_error);

        // Should not fail on empty address in free tier
        rpcparams = UniValue(UniValue::VARR);
        list = UniValue(UniValue::VARR);
        snode2 = UniValue(UniValue::VOBJ); snode2.pushKV("alias", "snode2"), snode2.pushKV("tier", "OPEN"), snode2.pushKV("address", "");
        list.push_back(snode1), list.push_back(snode2);
        rpcparams.push_backV({ "list", list });
        BOOST_CHECK_NO_THROW(CallRPC2("servicenodesetup", rpcparams));

        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test the servicenodesetup remove option
    {
        UniValue rpcparams(UniValue::VARR);
        rpcparams.push_backV({ "auto", 10, saddr });
        UniValue entries;
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesetup", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 10, "Service node config count should match expected");

        rpcparams = UniValue(UniValue::VARR);
        rpcparams.push_backV({ "remove" });
        BOOST_CHECK_NO_THROW(CallRPC2("servicenodesetup", rpcparams));
        std::set<sn::ServiceNodeConfigEntry> ent;
        sn::ServiceNodeMgr::instance().loadSnConfig(ent);
        BOOST_CHECK_MESSAGE(ent.empty(), "Service node setup remove option should result in 0 snode entries");

        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test servicenodegenkey rpc
    {
        UniValue result;
        BOOST_CHECK_NO_THROW(result = CallRPC2("servicenodegenkey", UniValue(UniValue::VARR)));
        BOOST_CHECK_MESSAGE(result.isStr(), "servicenodegenkey should return a string");
        CKey ckey = DecodeSecret(result.get_str());
        BOOST_CHECK_MESSAGE(ckey.IsValid(), "servicenodegenkey should return a valid private key");
    }

    // Test servicenodeexport and servicenodeimport rpc
    {
        UniValue entries;
        UniValue result;

        UniValue rpcparams(UniValue::VARR);
        rpcparams.push_backV({ "auto", 1, saddr });
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesetup", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 1, "Service node config count should match expected");

        const std::string & passphrase = "password";
        rpcparams = UniValue(UniValue::VARR);
        rpcparams.push_backV({ "snode0", passphrase });
        BOOST_CHECK_NO_THROW(result = CallRPC2("servicenodeexport", rpcparams));
        BOOST_CHECK_MESSAGE(result.isStr(), "servicenodeexport should return a string");

        // Check that the encrypted hex matches the expected output
        SecureString spassphrase; spassphrase.reserve(100);
        spassphrase = passphrase.c_str();
        CCrypter crypt;
        const std::vector<unsigned char> vchSalt = ParseHex("0000aabbccee0000"); // note* this salt is fixed (i.e. it's not being used)
        crypt.SetKeyFromPassphrase(spassphrase, vchSalt, 100, 0);
        CKeyingMaterial plaintext;
        BOOST_CHECK_MESSAGE(crypt.Decrypt(ParseHex(result.get_str()), plaintext), "servicenodeexport failed to decrypt plaintext");
        std::string strtext(plaintext.begin(), plaintext.end());
        const std::string & str = entries[0].write();
        BOOST_CHECK_EQUAL(strtext, str);

        // Check servicenodeimport
        rpcparams = UniValue(UniValue::VARR);
        rpcparams.push_backV({ result.get_str(), passphrase });
        BOOST_CHECK_NO_THROW(CallRPC2("servicenodeimport", rpcparams));
        BOOST_CHECK_MESSAGE(sn::ServiceNodeMgr::instance().getSnEntries().size() == 1, "servicenodeimport should have imported snode data");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();

        // Check servicenodeimport bad passphrase
        rpcparams = UniValue(UniValue::VARR);
        rpcparams.push_backV({ result.get_str(), "bad passphrase" });
        BOOST_CHECK_THROW(CallRPC2("servicenodeimport", rpcparams), std::runtime_error);
        BOOST_CHECK_MESSAGE(sn::ServiceNodeMgr::instance().getSnEntries().empty(), "servicenodeimport should fail due to bad passphrase");
        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test servicenodestatus and servicenodelist rpc
    {
        const auto tt = GetAdjustedTime();
        UniValue entries, o;

        UniValue rpcparams(UniValue::VARR);
        rpcparams.push_backV({ "auto", 1, saddr });
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesetup", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 1, "Service node config count should match expected");
        o = entries[0];
        const auto snodekey = find_value(o, "snodekey").get_str();
        const auto sk = DecodeSecret(snodekey);

        rpcparams = UniValue(UniValue::VARR);
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodestatus", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 1, "Service node status count should match expected");
        BOOST_CHECK_EQUAL(entries.isArray(), true);
        o = entries[0];
        BOOST_CHECK_EQUAL(find_value(o, "alias").get_str(), "snode0");
        BOOST_CHECK_EQUAL(find_value(o, "tier").get_str(), sn::ServiceNodeMgr::tierString(sn::ServiceNode::SPV));
        BOOST_CHECK_EQUAL(find_value(o, "snodekey").get_str(), snodekey);
        BOOST_CHECK_EQUAL(find_value(o, "address").get_str(), saddr);
        BOOST_CHECK      (find_value(o, "timeregistered").get_int() >= tt);
        BOOST_CHECK_EQUAL(find_value(o, "timelastseen").get_int(), 0);
        BOOST_CHECK_EQUAL(find_value(o, "timelastseenstr").get_str(), "1970-01-01T00:00:00Z");
        BOOST_CHECK_EQUAL(find_value(o, "status").get_str(), "offline"); // hasn't been started, expecting offline

        // Start the snode to add to list
        rpcparams = UniValue(UniValue::VARR);
        BOOST_CHECK_NO_THROW(CallRPC2("servicenoderegister", rpcparams));

        rpcparams = UniValue(UniValue::VARR);
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodelist", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 1, "Service node list count should match expected");
        BOOST_CHECK_EQUAL(entries.isArray(), true);
        o = entries[0];
        BOOST_CHECK_EQUAL(find_value(o, "snodekey").get_str(), HexStr(sk.GetPubKey()));
        BOOST_CHECK_EQUAL(find_value(o, "tier").get_str(), sn::ServiceNodeMgr::tierString(sn::ServiceNode::SPV));
        BOOST_CHECK_EQUAL(find_value(o, "address").get_str(), saddr);
        BOOST_CHECK      (find_value(o, "timeregistered").get_int() >= tt);
        BOOST_CHECK_EQUAL(find_value(o, "timelastseen").get_int(), 0);
        BOOST_CHECK_EQUAL(find_value(o, "timelastseenstr").get_str(), "1970-01-01T00:00:00Z");
        BOOST_CHECK_EQUAL(find_value(o, "status").get_str(), "running");

        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test servicenodesendping rpc
    {
        UniValue entries, o;

        UniValue rpcparams(UniValue::VARR);
        rpcparams.push_backV({ "auto", 1, saddr });
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesetup", rpcparams));
        BOOST_CHECK_MESSAGE(entries.size() == 1, "Service node config count should match expected");
        o = entries[0];
        const auto snodekey = find_value(o, "snodekey").get_str();
        const auto sk = DecodeSecret(snodekey);

        // First check error since snode is not started
        rpcparams = UniValue(UniValue::VARR);
        BOOST_CHECK_THROW(CallRPC2("servicenodesendping", rpcparams), std::runtime_error);

        // Start snode
        const auto tt2 = GetAdjustedTime();
        rpcparams = UniValue(UniValue::VARR);
        BOOST_CHECK_NO_THROW(CallRPC2("servicenoderegister", rpcparams));

        // Start snode and send ping
        BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodesendping", rpcparams));
        BOOST_CHECK_MESSAGE(entries.isObject(), "Service node ping should return the snode");
        o = entries.get_obj();
        BOOST_CHECK_EQUAL(find_value(o, "alias").get_str(), "snode0");
        BOOST_CHECK_EQUAL(find_value(o, "tier").get_str(), sn::ServiceNodeMgr::tierString(sn::ServiceNode::SPV));
        BOOST_CHECK_EQUAL(find_value(o, "snodekey").get_str(), HexStr(sk.GetPubKey()));
        BOOST_CHECK_EQUAL(find_value(o, "address").get_str(), saddr);
        BOOST_CHECK      (find_value(o, "timeregistered").get_int() >= tt2);
        BOOST_CHECK      (find_value(o, "timelastseen").get_int() >= tt2);
        BOOST_CHECK_EQUAL(find_value(o, "timelastseenstr").get_str().empty(), false);
        BOOST_CHECK_EQUAL(find_value(o, "status").get_str(), "running");

        sn::ServiceNodeMgr::writeSnConfig(std::vector<sn::ServiceNodeConfigEntry>(), false); // reset
        sn::ServiceNodeMgr::instance().reset();
    }

    // Test servicenodecreateinputs rpc
    {
        CKey key; key.MakeNewKey(true);
        CTxDestination dest(key.GetPubKey().GetID());

        // Send to other wallet key
        CReserveKey reservekey(pos.wallet.get());
        CAmount nFeeRequired;
        std::string strError;
        std::vector<CRecipient> vecSend;
        int nChangePosRet = -1;
        for (int i = 0; i < params->GetConsensus().snMaxCollateralCount*2; ++i) {
            CRecipient recipient = {GetScriptForDestination(dest),
                                    sn::ServiceNode::COLLATERAL_SPV/(params->GetConsensus().snMaxCollateralCount*2),
                                    false};
            vecSend.push_back(recipient);
        }
        // For fee
        vecSend.push_back({GetScriptForDestination(dest), COIN, false});
        CCoinControl cc;
        CTransactionRef tx;
        {
            auto locked_chain = pos.wallet->chain().lock();
            LOCK(pos.wallet->cs_wallet);
            BOOST_CHECK_MESSAGE(pos.wallet->CreateTransaction(*locked_chain, vecSend, tx, reservekey, nFeeRequired,
                                nChangePosRet, strError, cc), "Failed to send coin to other wallet");
            CValidationState state;
            BOOST_CHECK_MESSAGE(pos.wallet->CommitTransaction(tx, {}, {}, reservekey, g_connman.get(), state),
                                strprintf("Failed to send coin to other wallet: %s", state.GetRejectReason()));
        }
        pos.StakeBlocks(1), SyncWithValidationInterfaceQueue();

        // Create otherwallet to test create inputs rpc
        bool firstRun;
        auto otherwallet = std::make_shared<CWallet>(*pos.chain, WalletLocation(), WalletDatabase::CreateMock());
        otherwallet->LoadWallet(firstRun);
        AddKey(*otherwallet, key);
        AddWallet(otherwallet);
        otherwallet->SetBroadcastTransactions(true);
        rescanWallet(otherwallet.get());

        UniValue entries, o, rpcparams;

        {
            // Should fail on bad nodecount
            rpcparams = UniValue(UniValue::VARR);
            rpcparams.push_backV({ EncodeDestination(dest), -1 });
            BOOST_CHECK_THROW(CallRPC2("servicenodecreateinputs", rpcparams), std::runtime_error);

            // Should fail on missing nodecount
            rpcparams = UniValue(UniValue::VARR);
            rpcparams.push_backV({ EncodeDestination(dest) });
            BOOST_CHECK_THROW(CallRPC2("servicenodecreateinputs", rpcparams), std::runtime_error);

            // Should fail on bad address
            rpcparams = UniValue(UniValue::VARR);
            rpcparams.push_backV({ "kfdjsaklfjksdlajfkdsjfkldsjkfla", 1 });
            BOOST_CHECK_THROW(CallRPC2("servicenodecreateinputs", rpcparams), std::runtime_error);

            // Should fail on good address not in wallets
            CKey nk; nk.MakeNewKey(true);
            rpcparams = UniValue(UniValue::VARR);
            rpcparams.push_backV({ EncodeDestination(CTxDestination(nk.GetPubKey().GetID())), 1 });
            BOOST_CHECK_THROW(CallRPC2("servicenodecreateinputs", rpcparams), std::runtime_error);

            // Should fail on bad input size
            rpcparams = UniValue(UniValue::VARR);
            rpcparams.push_backV({ EncodeDestination(dest), 1, -1000 });
            BOOST_CHECK_THROW(CallRPC2("servicenodecreateinputs", rpcparams), std::runtime_error);

            // Should fail on bad input size
            rpcparams = UniValue(UniValue::VARR);
            rpcparams.push_backV({ EncodeDestination(dest), 1, 1000.123 });
            BOOST_CHECK_THROW(CallRPC2("servicenodecreateinputs", rpcparams), std::runtime_error);
        }

        // Test normal case (should succeed)
        {
            const int inputSize = static_cast<int>(sn::ServiceNode::COLLATERAL_SPV / COIN / params->GetConsensus().snMaxCollateralCount);
            rpcparams = UniValue(UniValue::VARR);
            rpcparams.push_backV({ EncodeDestination(dest), 1, inputSize });
            BOOST_CHECK_NO_THROW(entries = CallRPC2("servicenodecreateinputs", rpcparams));
            BOOST_CHECK_MESSAGE(entries.isObject(), "Bad result object");
            o = entries.get_obj();
            BOOST_CHECK_EQUAL(find_value(o, "nodecount").get_int(), 1);
            BOOST_CHECK_EQUAL(find_value(o, "collateral").get_int(), sn::ServiceNode::COLLATERAL_SPV / COIN);
            BOOST_CHECK_EQUAL(find_value(o, "inputsize").get_int(), inputSize);
            BOOST_CHECK_EQUAL(find_value(o, "txid").get_str().empty(), false);
            pos.StakeBlocks(1), SyncWithValidationInterfaceQueue();
            rescanWallet(otherwallet.get());

            // Check that tx was created
            const auto txid = uint256S(find_value(o, "txid").get_str());
            CTransactionRef txn;
            uint256 hashBlock;
            BOOST_CHECK_MESSAGE(GetTransaction(txid, txn, params->GetConsensus(), hashBlock), "Failed to find inputs tx");
            std::set<COutPoint> txvouts;
            CAmount txAmount{0};
            for (int i = 0; i < txn->vout.size(); ++i) {
                const auto & out = txn->vout[i];
                if (out.nValue != inputSize * COIN)
                    continue;
                txvouts.insert({txn->GetHash(), (uint32_t)i});
                txAmount += out.nValue;
            }
            // Check that coins in wallet match expected
            std::vector<COutput> coins;
            {
                LOCK2(cs_main, otherwallet->cs_wallet);
                otherwallet->AvailableCoins(*pos.locked_chain, coins);
            }
            std::vector<COutput> filtered;
            CAmount filteredAmount{0};
            for (const auto & coin : coins) {
                if (coin.GetInputCoin().txout.nValue != inputSize * COIN)
                    continue;
                filtered.push_back(coin);
                filteredAmount += coin.GetInputCoin().txout.nValue;
            }
            BOOST_CHECK_EQUAL(txvouts.size(), filtered.size());
            BOOST_CHECK_EQUAL(filtered.size(), params->GetConsensus().snMaxCollateralCount);
            BOOST_CHECK_EQUAL(txAmount, filteredAmount);
            BOOST_CHECK_EQUAL(filteredAmount, (CAmount)sn::ServiceNode::COLLATERAL_SPV);

            for (const auto & coin : filtered) {
                if (txvouts.count(coin.GetInputCoin().outpoint))
                    txvouts.erase(coin.GetInputCoin().outpoint);
            }
            BOOST_CHECK_EQUAL(txvouts.size(), 0); // expecting coinsdb to match transaction utxos
        }

        RemoveWallet(otherwallet);
    }
}

BOOST_AUTO_TEST_SUITE_END()