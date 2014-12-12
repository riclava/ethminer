/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file Executive.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include "Executive.h"

#include <boost/timer.hpp>
#include <libdevcore/CommonIO.h>
#include <libevm/VMFactory.h>
#include <libevm/VM.h>
#include "Interface.h"
#include "State.h"
#include "ExtVM.h"
using namespace std;
using namespace dev;
using namespace dev::eth;

#define ETH_VMTRACE 1

u256 Executive::gasUsed() const
{
	return m_t.gas() - m_endGas;
}

bool Executive::setup(bytesConstRef _rlp)
{
	// Entry point for a user-executed transaction.
	m_t = Transaction(_rlp);

	m_sender = m_t.sender();

	// Avoid invalid transactions.
	auto nonceReq = m_s.transactionsFrom(m_sender);
	if (m_t.nonce() != nonceReq)
	{
		clog(StateDetail) << "Invalid Nonce: Require" << nonceReq << " Got" << m_t.nonce();
		BOOST_THROW_EXCEPTION(InvalidNonce(nonceReq, m_t.nonce()));
	}

	// Check gas cost is enough.
	auto gasCost = Interface::txGas(m_t.data());

	if (m_t.gas() < gasCost)
	{
		clog(StateDetail) << "Not enough gas to pay for the transaction: Require >" << gasCost << " Got" << m_t.gas();
		BOOST_THROW_EXCEPTION(OutOfGas() << RequirementError((bigint)gasCost, (bigint)m_t.gas()));
	}

	u256 cost = m_t.value() + m_t.gas() * m_t.gasPrice();

	// Avoid unaffordable transactions.
	if (m_s.balance(m_sender) < cost)
	{
		clog(StateDetail) << "Not enough cash: Require >" << cost << " Got" << m_s.balance(m_sender);
		BOOST_THROW_EXCEPTION(NotEnoughCash() << RequirementError((bigint)cost, (bigint)m_s.balance(m_sender)));
	}

	u256 startGasUsed = m_s.gasUsed();
	if (startGasUsed + m_t.gas() > m_s.m_currentBlock.gasLimit)
	{
		clog(StateDetail) << "Too much gas used in this block: Require <" << (m_s.m_currentBlock.gasLimit - startGasUsed) << " Got" << m_t.gas();
		BOOST_THROW_EXCEPTION(BlockGasLimitReached() << RequirementError((bigint)(m_s.m_currentBlock.gasLimit - startGasUsed), (bigint)m_t.gas()));
	}

	// Increment associated nonce for sender.
	m_s.noteSending(m_sender);

	// Pay...
	clog(StateDetail) << "Paying" << formatBalance(cost) << "from sender (includes" << m_t.gas() << "gas at" << formatBalance(m_t.gasPrice()) << ")";
	m_s.subBalance(m_sender, cost);

	if (m_t.isCreation())
		return create(m_sender, m_t.value(), m_t.gasPrice(), m_t.gas() - (u256)gasCost, &m_t.data(), m_sender);
	else
		return call(m_t.receiveAddress(), m_t.receiveAddress(), m_sender, m_t.value(), m_t.gasPrice(), bytesConstRef(&m_t.data()), m_t.gas() - (u256)gasCost, m_sender);
}

bool Executive::call(Address _receiveAddress, Address _codeAddress, Address _senderAddress, u256 _value, u256 _gasPrice, bytesConstRef _data, u256 _gas, Address _originAddress)
{
	m_isCreation = false;
//	cnote << "Transferring" << formatBalance(_value) << "to receiver.";
	m_s.addBalance(_receiveAddress, _value);

	auto it = !(_receiveAddress & ~h160(0xffffffff)) ? State::precompiled().find((unsigned)(u160)_receiveAddress) : State::precompiled().end();
	if (it != State::precompiled().end())
	{
		bigint g = it->second.gas(_data);
		if (_gas < g)
		{
			m_endGas = 0;
			return false;
		}
		m_endGas = (u256)(_gas - g);
		it->second.exec(_data, bytesRef());
		return true;
	}
	else if (m_s.addressHasCode(_codeAddress))
	{
		m_vm = VMFactory::create(_gas);
		bytes const& c = m_s.code(_receiveAddress);
		m_ext = make_shared<ExtVM>(m_s, _receiveAddress, _senderAddress, _originAddress, _value, _gasPrice, _data, &c, m_depth);
	}
	else
		m_endGas = _gas;
	return !m_ext;
}

bool Executive::create(Address _sender, u256 _endowment, u256 _gasPrice, u256 _gas, bytesConstRef _init, Address _origin)
{
	m_isCreation = true;

	// We can allow for the reverted state (i.e. that with which m_ext is constructed) to contain the m_newAddress, since
	// we delete it explicitly if we decide we need to revert.
	m_newAddress = right160(sha3(rlpList(_sender, m_s.transactionsFrom(_sender) - 1)));

	// Set up new account...
	m_s.m_cache[m_newAddress] = Account(m_s.balance(m_newAddress) + _endowment, Account::ContractConception);

	// Execute _init.
	m_vm = VMFactory::create(_gas);
	m_ext = make_shared<ExtVM>(m_s, m_newAddress, _sender, _origin, _endowment, _gasPrice, bytesConstRef(), _init, m_depth);
	return _init.empty();
}

OnOpFunc Executive::simpleTrace()
{
	return [](uint64_t steps, Instruction inst, bigint newMemSize, bigint gasCost, VM* voidVM, ExtVMFace const* voidExt)
	{
		ExtVM const& ext = *static_cast<ExtVM const*>(voidExt);
		VM& vm = *voidVM;

		ostringstream o;
		o << endl << "    STACK" << endl;
		for (auto i: vm.stack())
			o << (h256)i << endl;
		o << "    MEMORY" << endl << memDump(vm.memory());
		o << "    STORAGE" << endl;
		for (auto const& i: ext.state().storage(ext.myAddress))
			o << showbase << hex << i.first << ": " << i.second << endl;
		dev::LogOutputStream<VMTraceChannel, false>(true) << o.str();
		dev::LogOutputStream<VMTraceChannel, false>(false) << " | " << dec << ext.depth << " | " << ext.myAddress << " | #" << steps << " | " << hex << setw(4) << setfill('0') << vm.curPC() << " : " << instructionInfo(inst).name << " | " << dec << vm.gas() << " | -" << dec << gasCost << " | " << newMemSize << "x32" << " ]";
	};
}

bool Executive::go(OnOpFunc const& _onOp)
{
	if (m_vm)
	{
		boost::timer t;
		auto sgas = m_vm->gas();
		try
		{
			m_out = m_vm->go(*m_ext, _onOp, 0);
			m_endGas = m_vm->gas();
			m_endGas += min((m_t.gas() - m_endGas) / 2, m_ext->sub.refunds);
			m_logs = m_ext->sub.logs;

			if (m_isCreation)
			{
				if (m_out.size() * c_createDataGas <= m_endGas)
					m_endGas -= m_out.size() * c_createDataGas;
				else
					m_out.reset();
			}
		}
		catch (StepsDone const&)
		{
			return false;
		}
		catch (VMException const& _e)
		{
			clog(StateChat) << "Safe VM Exception: " << diagnostic_information(_e);
			m_endGas = 0;//m_vm->gas();

			// Write state out only in the case of a non-excepted transaction.
			m_ext->revert();

			m_excepted = true;
		}
		catch (Exception const& _e)
		{
			// TODO: AUDIT: check that this can never reasonably happen. Consider what to do if it does.
			cwarn << "Unexpected exception in VM. There may be a bug in this implementation. " << diagnostic_information(_e);
		}
		catch (std::exception const& _e)
		{
			// TODO: AUDIT: check that this can never reasonably happen. Consider what to do if it does.
			cwarn << "Unexpected std::exception in VM. This is probably unrecoverable. " << _e.what();
		}
		cnote << "VM took:" << t.elapsed() << "; gas used: " << (sgas - m_endGas);
	}
	return true;
}

u256 Executive::gas() const
{
	return m_vm ? m_vm->gas() : m_endGas;
}

void Executive::finalize(OnOpFunc const&)
{
	if (m_t.isCreation() && !m_ext->sub.suicides.count(m_newAddress))
		// creation - put code in place.
		m_s.m_cache[m_newAddress].setCode(m_out);

	//	cnote << "Refunding" << formatBalance(m_endGas * m_ext->gasPrice) << "to origin (=" << m_endGas << "*" << formatBalance(m_ext->gasPrice) << ")";
	m_s.addBalance(m_sender, m_endGas * m_t.gasPrice());

	u256 feesEarned = (m_t.gas() - m_endGas) * m_t.gasPrice();
//	cnote << "Transferring" << formatBalance(gasSpent) << "to miner.";
	m_s.addBalance(m_s.m_currentBlock.coinbaseAddress, feesEarned);

	// Suicides...
	if (m_ext)
		for (auto a: m_ext->sub.suicides)
			m_s.m_cache[a].kill();
}
