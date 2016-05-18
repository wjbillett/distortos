/**
 * \file
 * \brief SerialPort class implementation
 *
 * \author Copyright (C) 2016 Kamil Szczygiel http://www.distortec.com http://www.freddiechopin.info
 *
 * \par License
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "distortos/devices/communication/SerialPort.hpp"

#include "distortos/internal/devices/UartLowLevel.hpp"

#include "distortos/architecture/InterruptMaskingLock.hpp"

#include "distortos/Semaphore.hpp"

#include "estd/ScopeGuard.hpp"

#include <cstring>

namespace distortos
{

namespace devices
{

namespace
{

/*---------------------------------------------------------------------------------------------------------------------+
| local functions
+---------------------------------------------------------------------------------------------------------------------*/

/**
 * \brief Reads data from ring buffer.
 *
 * \param [in] ringBuffer is a reference to ring buffer from which the data will be read
 * \param [out] buffer is a buffer to which the data will be written
 * \param [in] size is the size of \a buffer, bytes
 *
 * \return number of bytes read from \a ringBuffer and written to \a buffer
 */

size_t readFromRingBuffer(SerialPort::RingBuffer& ringBuffer, uint8_t* const buffer, const size_t size)
{
	decltype(ringBuffer.getReadBlock()) readBlock;
	size_t bytesRead {};
	while (readBlock = ringBuffer.getReadBlock(), readBlock.second != 0 && bytesRead != size)
	{
		const auto copySize = std::min(readBlock.second, size - bytesRead);
		memcpy(buffer + bytesRead, readBlock.first, copySize);
		ringBuffer.increaseReadPosition(copySize);
		bytesRead += copySize;
	}

	return bytesRead;
}

/**
 * \brief Writes data to ring buffer.
 *
 * \param [in] buffer is a buffer from which the data will be read
 * \param [in] size is the size of \a buffer, bytes
 * \param [in] ringBuffer is a reference to ring buffer to which the data will be written
 *
 * \return number of bytes read from \a buffer and written to \a ringBuffer
 */

size_t writeToRingBuffer(const uint8_t* const buffer, const size_t size, SerialPort::RingBuffer& ringBuffer)
{
	decltype(ringBuffer.getWriteBlock()) writeBlock;
	size_t bytesWritten {};
	while (writeBlock = ringBuffer.getWriteBlock(), writeBlock.second != 0 && bytesWritten != size)
	{
		const auto copySize = std::min(writeBlock.second, size - bytesWritten);
		memcpy(writeBlock.first, buffer + bytesWritten, copySize);
		ringBuffer.increaseWritePosition(copySize);
		bytesWritten += copySize;
	}

	return bytesWritten;
}

}	// namespace

/*---------------------------------------------------------------------------------------------------------------------+
| SerialPort::RingBuffer public functions
+---------------------------------------------------------------------------------------------------------------------*/

std::pair<const uint8_t*, size_t> SerialPort::RingBuffer::getReadBlock() const
{
	const auto readPosition = readPosition_;
	const auto writePosition = writePosition_;
	return {buffer_ + readPosition, (writePosition >= readPosition ? writePosition : size_) - readPosition};
}

std::pair<uint8_t*, size_t> SerialPort::RingBuffer::getWriteBlock() const
{
	const auto readPosition = readPosition_;
	const auto writePosition = writePosition_;
	const auto freeBytes = (readPosition > writePosition ? readPosition - writePosition :
			size_ - writePosition + readPosition) - 2;
	const auto writeBlockSize = (readPosition > writePosition ? readPosition : size_) - writePosition;
	return {buffer_ + writePosition, std::min(freeBytes, writeBlockSize)};
}

/*---------------------------------------------------------------------------------------------------------------------+
| public functions
+---------------------------------------------------------------------------------------------------------------------*/

SerialPort::~SerialPort()
{
	if (openCount_ == 0)
		return;

	readMutex_.lock();
	writeMutex_.lock();
	const auto readWriteMutexScopeGuard = estd::makeScopeGuard(
			[this]()
			{
				writeMutex_.unlock();
				readMutex_.unlock();
			});

	uart_.stopRead();
	uart_.stopWrite();
	uart_.stop();
}

int SerialPort::close()
{
	readMutex_.lock();
	writeMutex_.lock();
	const auto readWriteMutexScopeGuard = estd::makeScopeGuard(
			[this]()
			{
				writeMutex_.unlock();
				readMutex_.unlock();
			});

	if (openCount_ == 0)	// device is not open anymore?
		return EBADF;

	if (openCount_ == 1)	// last close?
	{
		while (transmitInProgress_ == true)	// wait for physical end of write operation
		{
			Semaphore semaphore {0};
			transmitSemaphore_ = &semaphore;
			const auto transmitSemaphoreScopeGuard = estd::makeScopeGuard(
					[this]()
					{
						transmitSemaphore_ = {};
					});

			if (transmitInProgress_ == true)
			{
				const auto ret = semaphore.wait();
				if (ret != 0)
					return ret;
			}
		}

		uart_.stopRead();

		const auto ret = uart_.stop();
		if (ret != 0)
			return ret;

		readBuffer_.clear();
		writeBuffer_.clear();
	}

	--openCount_;
	return 0;
}

int SerialPort::open(const uint32_t baudRate, const uint8_t characterLength, const devices::UartParity parity,
			const bool _2StopBits)
{
	readMutex_.lock();
	writeMutex_.lock();
	const auto readWriteMutexScopeGuard = estd::makeScopeGuard(
			[this]()
			{
				writeMutex_.unlock();
				readMutex_.unlock();
			});

	if (openCount_ == std::numeric_limits<decltype(openCount_)>::max())	// device is already opened too many times?
		return EMFILE;

	if (openCount_ == 0)	// first open?
	{
		if (readBuffer_.getSize() < 4 || writeBuffer_.getSize() < 4)
			return ENOBUFS;

		{
			const auto ret = uart_.start(*this, baudRate, characterLength, parity, _2StopBits);
			if (ret.first != 0)
				return ret.first;
		}
		{
			const auto ret = startReadWrapper(SIZE_MAX);
			if (ret != 0)
				return ret;
		}

		baudRate_ = baudRate;
		characterLength_ = characterLength;
		parity_ = parity;
		_2StopBits_ = _2StopBits;
	}
	else	// if (openCount_ != 0)
	{
		// provided arguments don't match current configuration of already opened device?
		if (baudRate_ != baudRate || characterLength_ != characterLength || parity_ != parity ||
				_2StopBits_ != _2StopBits)
			return EINVAL;
	}

	++openCount_;
	return 0;
}

std::pair<int, size_t> SerialPort::read(void* const buffer, const size_t size)
{
	if (buffer == nullptr || size == 0)
		return {EINVAL, {}};

	readMutex_.lock();
	const auto readMutexScopeGuard = estd::makeScopeGuard(
			[this]()
			{
				readMutex_.unlock();
			});

	if (openCount_ == 0)
		return {EBADF, {}};

	if (characterLength_ > 8 && size % 2 != 0)
		return {EINVAL, {}};

	const size_t minSize = characterLength_ <= 8 ? 1 : 2;
	const auto bufferUint8 = static_cast<uint8_t*>(buffer);
	size_t bytesRead {};
	while (bytesRead < minSize)
	{
		bytesRead += readFromRingBuffer(readBuffer_, bufferUint8 + bytesRead, size - bytesRead);
		if (bytesRead == size)	// buffer already full?
			return {{}, bytesRead};

		Semaphore semaphore {0};
		readSemaphore_ = &semaphore;
		const auto readSemaphoreScopeGuard = estd::makeScopeGuard(
				[this]()
				{
					readSemaphore_ = {};
				});

		{
			// stop and restart the read operation to get the characters that were already received;
			architecture::InterruptMaskingLock interruptMaskingLock;
			const auto bytesReceived = uart_.stopRead();
			readBuffer_.increaseWritePosition(bytesReceived);
			// limit of new read operation is selected to have a notification when requested minimum will be received
			const auto ret = startReadWrapper(minSize > bytesRead + bytesReceived ?
					minSize - bytesRead - bytesReceived : SIZE_MAX);
			if (ret != 0)
				return {ret, bytesRead};
		}

		bytesRead += readFromRingBuffer(readBuffer_, bufferUint8 + bytesRead, size - bytesRead);

		if (bytesRead < minSize)	// wait for data only if requested minimum is not already read
		{
			const auto ret = semaphore.wait();
			if (ret != 0)
				return {ret, bytesRead};
		}
	}

	return {{}, bytesRead};
}

std::pair<int, size_t> SerialPort::write(const void* const buffer, const size_t size)
{
	if (buffer == nullptr || size == 0)
		return {EINVAL, {}};

	writeMutex_.lock();
	const auto writeMutexScopeGuard = estd::makeScopeGuard(
			[this]()
			{
				writeMutex_.unlock();
			});

	if (openCount_ == 0)
		return {EBADF, {}};

	if (characterLength_ > 8 && size % 2 != 0)
		return {EINVAL, {}};

	const auto bufferUint8 = static_cast<const uint8_t*>(buffer);
	size_t bytesWritten {};
	while (bytesWritten < size)
	{
		Semaphore semaphore {0};
		writeSemaphore_ = &semaphore;
		const auto writeSemaphoreScopeGuard = estd::makeScopeGuard(
				[this]()
				{
					writeSemaphore_ = {};
				});

		bytesWritten += writeToRingBuffer(bufferUint8 + bytesWritten, size - bytesWritten, writeBuffer_);

		// restart write operation if it is not currently in progress and the write buffer is not already empty
		if (writeInProgress_ == false && writeBuffer_.isEmpty() == false)
		{
			const auto ret = startWriteWrapper();
			if (ret != 0)
				return {ret, bytesWritten};
		}
		// wait for free space only if write operation is in progress and there is still some data left to write
		else if (bytesWritten != size)
		{
			const auto ret = semaphore.wait();
			if (ret != 0)
				return {ret, bytesWritten};
		}
	}

	return {{}, bytesWritten};
}

/*---------------------------------------------------------------------------------------------------------------------+
| private functions
+---------------------------------------------------------------------------------------------------------------------*/

void SerialPort::readCompleteEvent(const size_t bytesRead)
{
	readBuffer_.increaseWritePosition(bytesRead);

	if (readSemaphore_ != nullptr)
	{
		readSemaphore_->post();
		readSemaphore_ = {};
	}

	if (readBuffer_.isFull() == true)
		return;

	startReadWrapper(SIZE_MAX);
}

void SerialPort::receiveErrorEvent(ErrorSet)
{

}

int SerialPort::startReadWrapper(const size_t limit)
{
	const auto writeBlock = readBuffer_.getWriteBlock();
	return uart_.startRead(writeBlock.first, std::min({writeBlock.second, readBuffer_.getSize() / 2, limit}));
}

int SerialPort::startWriteWrapper()
{
	transmitInProgress_ = true;
	writeInProgress_ = true;
	const auto outBlock = writeBuffer_.getReadBlock();
	return uart_.startWrite(outBlock.first, outBlock.second);
}

void SerialPort::transmitCompleteEvent()
{
	if (transmitSemaphore_ != nullptr)
	{
		transmitSemaphore_->post();
		transmitSemaphore_ = {};
	}

	transmitInProgress_ = false;
}

void SerialPort::writeCompleteEvent(const size_t bytesWritten)
{
	writeBuffer_.increaseReadPosition(bytesWritten);

	if (writeSemaphore_ != nullptr)
	{
		writeSemaphore_->post();
		writeSemaphore_ = {};
	}

	if (writeBuffer_.isEmpty() == true)
	{
		writeInProgress_ = false;
		return;
	}

	startWriteWrapper();
}

}	// namespace devices

}	// namespace distortos