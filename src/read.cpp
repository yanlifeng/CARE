#include "../inc/read.hpp"


	Sequence::Sequence() : nBases(0)
	{
		data.second = 0;
	}

	Sequence::Sequence(const std::string& sequence) 
		: nBases(sequence.length())
	{
		data = encode_2bit(sequence);
	}

	Sequence::Sequence(const std::uint8_t* rawdata, int nBases_) 
		: nBases(nBases_)
	{
		const int size = SDIV(nBases,4);
		data.first = std::make_unique<std::uint8_t[]>(size);
		data.second = size;

		std::copy(rawdata, rawdata + size, begin());
	}

	Sequence::Sequence(Sequence&& other)
	{
		*this = std::move(other);
	}

	Sequence::Sequence(const Sequence& other)
	{
		*this = other;
	}

	Sequence& Sequence::operator=(const Sequence& other)
	{
		nBases = other.nBases;

		const int size = other.getNumBytes();
		data.first = std::make_unique<std::uint8_t[]>(size);
		data.second = size;

		std::copy(other.begin(), other.end(), begin());

		return *this;
	}

	Sequence& Sequence::operator=(Sequence&& other){
		if(this != &other){
			nBases = other.nBases;

			data = std::move(other.data);

			other.nBases = 0;
			other.data.second = 0;
		}
	        return *this;
	}

	bool Sequence::operator==(const Sequence& rhs) const
	{
		if(getNbases() != rhs.getNbases()) return false;
		return (std::memcmp(begin(), rhs.begin(), getNumBytes()) == 0);
	}

	bool Sequence::operator!=(const Sequence& other) const
	{
		return !(*this == other);
	}

	bool Sequence::operator==(const std::string& other) const
	{
		return toString() == other;
	}

	bool Sequence::operator!=(const std::string& other) const
	{
		return !(*this == other);
	}

	char Sequence::operator[](int i) const
	{
		const int UNUSED_BYTE_SPACE = 4 - (nBases % 4);

		const int byte = (i + UNUSED_BYTE_SPACE) / 4;
		const int basepos = (i + UNUSED_BYTE_SPACE) % 4;

		switch ((data.first[byte] >> (3 - basepos) * 2) & 0x03) {
		case BASE_A: return 'A';
		case BASE_C: return 'C';
		case BASE_G: return 'G';
		case BASE_T: return 'T';
		default: return '_';         // cannot happen
		}
	}

	std::string Sequence::toString() const
	{
		return decode_2bit(data.first, nBases);
	}

	bool Sequence::operator<(const Sequence& rhs) const{
		const int bases = getNbases();
		const int otherbases = rhs.getNbases();
		if(bases < otherbases) return true;
		if(bases > otherbases) return false;

		return (std::memcmp(begin(), rhs.begin(), getNumBytes()) < 0);
	}

	Sequence Sequence::reverseComplement() const{
		Sequence revcompl;
		revcompl.nBases = getNbases();
		revcompl.data.first.reset(new std::uint8_t[getNumBytes()]);
		revcompl.data.second = getNumBytes();

		bool res = encoded_to_reverse_complement_encoded(begin(), getNumBytes(), revcompl.begin(), getNumBytes(), getNbases());
		if(!res)
			throw std::runtime_error("could not get reverse complement of " + toString());
		
		return revcompl;
	}

	int Sequence::getNumBytes() const{
		return data.second;	
	}

	int Sequence::getNbases() const{
		return nBases;
	}

	bool Sequence::isCompressed() const{
		return true;
	}

	std::uint8_t* Sequence::begin() const{
		return data.first.get();
	}

	std::uint8_t* Sequence::end() const{
		return data.first.get() + getNumBytes();
	}

	std::ostream& operator<<(std::ostream& stream, const Sequence& seq){
		stream << seq.toString();
		return stream;
	}










	SequenceGeneral::SequenceGeneral() : nBases(0), compressed(false)
	{
	}

	SequenceGeneral::SequenceGeneral(const std::string& sequence, bool saveCompressed) 
		: nBases(sequence.length()), compressed(saveCompressed)
	{

		if(saveCompressed){
			data = encode_2bit(sequence);			
		}else{
			data.first = std::make_unique<std::uint8_t[]>(nBases);
			data.second = nBases;
			std::copy(sequence.begin(), sequence.end(), begin());
		}
	}

	SequenceGeneral::SequenceGeneral(const std::uint8_t* rawdata, int nBases_, bool isCompressed) 
		: nBases(nBases_), compressed(isCompressed)
	{
		const int size = compressed ? SDIV(nBases,4) : nBases;
		data.first = std::make_unique<std::uint8_t[]>(size);
		data.second = size;

		std::copy(rawdata, rawdata + size, begin());
	}

	SequenceGeneral::SequenceGeneral(SequenceGeneral&& other)
	{
		*this = std::move(other);
	}

	SequenceGeneral::SequenceGeneral(const SequenceGeneral& other)
	{
		*this = other;
	}

	SequenceGeneral& SequenceGeneral::operator=(const SequenceGeneral& other)
	{
		nBases = other.nBases;
		compressed = other.compressed;

		const int size = other.getNumBytes();
		data.first = std::make_unique<std::uint8_t[]>(size);
		data.second = size;

		std::copy(other.begin(), other.end(), begin());

		return *this;
	}

	SequenceGeneral& SequenceGeneral::operator=(SequenceGeneral&& other){
		if(this != &other){
			nBases = other.nBases;
			compressed = other.compressed;

			data = std::move(other.data);

			other.nBases = 0;
			other.data.second = 0;
		}
	        return *this;
	}

	bool SequenceGeneral::operator==(const SequenceGeneral& rhs) const
	{
		if(getNbases() != rhs.getNbases()) return false;

		if(isCompressed() == rhs.isCompressed())
			return (std::memcmp(begin(), rhs.begin(), getNumBytes()) == 0);
		else{
			for(int i = 0; i < getNbases(); i++){
				if (begin()[i] != rhs.begin()[i])
					return false;
			}
			return true;
		}
	}

	bool SequenceGeneral::operator!=(const SequenceGeneral& other) const
	{
		return !(*this == other);
	}

	bool SequenceGeneral::operator==(const std::string& other) const
	{
		return toString() == other;
	}

	bool SequenceGeneral::operator!=(const std::string& other) const
	{
		return !(*this == other);
	}

	char SequenceGeneral::operator[](int i) const
	{
		if(compressed){
			const int UNUSED_BYTE_SPACE = 4 - (nBases % 4);

			const int byte = (i + UNUSED_BYTE_SPACE) / 4;
			const int basepos = (i + UNUSED_BYTE_SPACE) % 4;

			switch ((data.first[byte] >> (3 - basepos) * 2) & 0x03) {
			case BASE_A: return 'A';
			case BASE_C: return 'C';
			case BASE_G: return 'G';
			case BASE_T: return 'T';
			default: return '_';         // cannot happen
			}
		}else{
			return char(data.first[i]);
		}

	}

	std::string SequenceGeneral::toString() const
	{
		if(compressed){
			return decode_2bit(data.first, nBases);
		}else{
			return std::string(reinterpret_cast<char*>(begin()), nBases);
		}
	}

	bool SequenceGeneral::operator<(const SequenceGeneral& rhs) const{
		const int bases = getNbases();
		const int otherbases = rhs.getNbases();
		if(bases < otherbases) return true;
		if(bases > otherbases) return false;

		if(isCompressed() == rhs.isCompressed())
			return (std::memcmp(begin(), rhs.begin(), getNumBytes()) < 0);
		else{
			for(int i = 0; i < bases; i++){
				if (begin()[i] < rhs.begin()[i])
					return true;
				if (begin()[i] > rhs.begin()[i])
					return false;
			}
			return false;
		}
	}

	SequenceGeneral SequenceGeneral::reverseComplement() const{
		if(isCompressed()){
			SequenceGeneral revcompl;
			revcompl.nBases = getNbases();
			revcompl.compressed = true;
			revcompl.data.first.reset(new std::uint8_t[getNumBytes()]);
			revcompl.data.second = getNumBytes();

			bool res = encoded_to_reverse_complement_encoded(begin(), getNumBytes(), revcompl.begin(), getNumBytes(), getNbases());
			if(!res)
				throw std::runtime_error("could not get reverse complement of " + toString());
			
			return revcompl;
		}else{
			SequenceGeneral revcompl;
			revcompl.nBases = getNbases();
			revcompl.compressed = false;
			revcompl.data.first.reset(new std::uint8_t[getNumBytes()]);
			revcompl.data.second = getNumBytes();

			std::reverse_copy(begin(), end(), revcompl.begin());

			for(char* p = (char*)revcompl.begin(); p < (char*)revcompl.end(); p++){
				switch(*p){
					case 'A': *p = 'T'; break;
					case 'C': *p = 'G'; break;
					case 'G': *p = 'C'; break;
					case 'T': *p = 'A'; break;
					default : break; // don't change N
				}
			}

			return revcompl;
		}
	}

	int SequenceGeneral::getNumBytes() const{
		return data.second;	
	}

	int SequenceGeneral::getNbases() const{
		return nBases;
	}

	bool SequenceGeneral::isCompressed() const{
		return compressed;
	}

	std::uint8_t* SequenceGeneral::begin() const{
		return data.first.get();
	}

	std::uint8_t* SequenceGeneral::end() const{
		return data.first.get() + getNumBytes();
	}

	std::ostream& operator<<(std::ostream& stream, const SequenceGeneral& seq){
		stream << seq.toString();
		return stream;
	}
