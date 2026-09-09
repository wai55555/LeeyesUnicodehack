#pragma once
template < typename T >
class com_ptr
{

	T* m_pointer;
	int m_refCount;
	void AddRef(T* ptr)
	{
		ptr->AddRef();
	}
	void Release(T* ptr)
	{
		ptr->Release();
	}
public:
//	com_ptr():m_pointer(NULL),m_refCount(0){};
	explicit com_ptr(T* pointer=NULL, bool AddReferenceCount= false):m_pointer(NULL),m_refCount(0)
	{
		if( AddReferenceCount && pointer )
		{
			AddRef(pointer);
		} 
		m_pointer=pointer;
		if( pointer )
		{
			++m_refCount;
		}

	}
	
	com_ptr( com_ptr& ptr)
	{
		if( ptr.m_pointer )
		{
			AddRef(ptr.m_pointer);
			ptr.m_refCount++;
		}
	
		m_pointer=ptr.m_pointer;
		m_refCount=ptr.m_refCount;

	}
	~com_ptr()
	{
		if( m_pointer )
		{	
			Release(m_pointer);
			--m_refCount;
		}
	}
	T* get()const{ return m_pointer; }
	T*const* getpp()const{ return  &m_pointer; }
    operator T*() const throw()
    {
        return m_pointer;
    }
    T& operator*() const
    {
        return *m_pointer;
    }

	com_ptr& operator=( com_ptr& srcp)
	{ 
		if( m_pointer )
		{
			Release(m_pointer);
			--m_refCount;
		}
		if( srcp.m_pointer )
		{
			AddRef(srcp.m_pointer);
			srcp.m_refCount++;
		}

		m_pointer = srcp.m_pointer;
		m_refCount = srcp.m_refCount;
		return *this;
	}

	void reset(T* p)
	{
		if( m_pointer )
		{
			Release(m_pointer);
			--m_refCount;
		}
		m_pointer = p;
		m_refCount = 0;
	}
	T** ToCreator()
	{
	   // 自分のインターフェイスを変更する事が前提
	   if(m_pointer)
	   {
		  Release(m_pointer);
	   }
	   m_pointer = NULL;
	   m_refCount=1;
	   return &m_pointer;
	}

	T*const operator->()const{ return m_pointer; }

	   // =代入演算子（インターフェイス登録）
   void operator =(T* pInterface)
   {
      // 自分のインターフェイスの参照カウンタを1つ減らす
      if(m_pointer)
	  {
         Release(m_pointer);
	  }

      // コピー
      m_pointer = pInterface;
	  m_refCount = 1;
   }

   // !=比較演算子
   BOOL operator !=(int val)
   {
      if(val != (int)m_pointer)
	  {
         return TRUE;
	  }
      return FALSE;
   }

   // ==比較演算子
   BOOL operator ==(int val)
   {
      if(val == (int)m_pointer)
	  {
         return TRUE;
	  }
      return FALSE;
   }

	//explicit operator bool() const
	//{
	//	return m_pointer;
	//}
    //typedef com_ptr<T> this_type;
    //typedef T* (this_type::*unspecified_bool_type)() const;
    //
    //operator unspecified_bool_type() const
    //{
    //    return m_pointer ? &this_type::get : 0;
    //}
//	bool operator !()const{return m_pointer == NULL; }
	 bool operator!() const throw()
    {	
        return (m_pointer == NULL);
    }
   bool operator<(_In_opt_ T* pT) const throw()
    {
        return m_pointer < pT;
    }
    bool operator!=(_In_opt_ T* pT) const
    {
        return !operator==(pT);
    }
    bool operator==(_In_opt_ T* pT) const throw()
    {
        return m_pointer == pT;
    }
};
