/* Copyright (c) 2025-2026 Hans-Kristian Arntzen for Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <utility>

template <typename T>
class ComPtr
{
public:
	ComPtr() = default;
	~ComPtr() { release(); }

	ComPtr(const ComPtr &other) { *this = other; }
	ComPtr &operator=(const ComPtr &other);
	ComPtr &operator=(ComPtr &&other) noexcept;

	ComPtr(ComPtr &&other) noexcept { *this = std::move(other); }
	ComPtr &operator=(T *ptr_) { release(); ptr = ptr_; return *this; }

	T *operator->() { return ptr; }
	T *get() const { return ptr; }
	void **ppv() { release(); return reinterpret_cast<void **>(&ptr); }

	void operator&() = delete;
	explicit operator bool() const { return ptr != nullptr; }

	static ComPtr create() { return ComPtr(new T); }
	static ComPtr addref(T *ptr_) { ptr_->AddRef(); return ComPtr(ptr_); }

private:
	T *ptr = nullptr;
	void release()
	{
		if (ptr)
			ptr->Release();
		ptr = nullptr;
	}
	ComPtr *self_addr() { return this; }
	const ComPtr *self_addr() const { return this; }
	ComPtr(T *ptr_) : ptr(ptr_) {}
};

template <typename T>
ComPtr<T> &ComPtr<T>::operator=(const ComPtr &other)
{
	if (this == other.self_addr())
		return *this;
	if (other.ptr)
		other.ptr->AddRef();
	release();
	ptr = other.ptr;
	return *this;
}

template <typename T>
ComPtr<T> &ComPtr<T>::operator=(ComPtr &&other) noexcept
{
	if (this == other.self_addr())
		return *this;
	release();
	if (other.ptr)
		ptr = other.ptr;
	other.ptr = nullptr;
	return *this;
}

