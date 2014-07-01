#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>

#include <boost/bind.hpp>

#include <xercesc/sax2/Attributes.hpp>
#include <xercesc/dom/DOM.hpp>

#include "FWCore/MessageLogger/interface/MessageLogger.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/Utilities/interface/Exception.h"
#include "FWCore/Utilities/interface/TimeOfDay.h"

#include "GeneratorInterface/LHEInterface/interface/LHEReader.h"
#include "GeneratorInterface/LHEInterface/interface/LHERunInfo.h"
#include "GeneratorInterface/LHEInterface/interface/LHEEvent.h"

#include "Utilities/StorageFactory/interface/IOTypes.h"
#include "Utilities/StorageFactory/interface/Storage.h"
#include "Utilities/StorageFactory/interface/StorageFactory.h"

#include "XMLUtils.h"

#include "boost/lexical_cast.hpp"

XERCES_CPP_NAMESPACE_USE

namespace lhef {

  static void logFileAction(char const* msg, std::string const& fileName) {
    edm::LogAbsolute("fileAction") << std::setprecision(0) << edm::TimeOfDay() << msg << fileName;
    edm::FlushMessageLog();
  }


class LHEReader::Source {
    public:
	Source() {}
	virtual ~Source() {}
	virtual XMLDocument *createReader(XMLDocument::Handler &handler) = 0;
};

class LHEReader::FileSource : public LHEReader::Source {
    public:
	FileSource(const std::string &fileURL)
	{
		Storage *storage =
			StorageFactory::get()->open(fileURL,
			                            IOFlags::OpenRead);

		if (!storage)
			throw cms::Exception("FileOpenError")
				<< "Could not open LHE file \""
				<< fileURL << "\" for reading"
				<< std::endl;

		fileStream.reset(new StorageWrap(storage));
	}

	~FileSource() {}

	XMLDocument *createReader(XMLDocument::Handler &handler) override
	{ return new XMLDocument(fileStream, handler); }

    private:
	std::auto_ptr<StorageWrap>	fileStream;
};

class LHEReader::StringSource : public LHEReader::Source {
    public:
	StringSource(const std::string &inputs)
	{
		if (inputs == "")
			throw cms::Exception("StreamOpenError")
				<< "Empty LHE file string name \""
				<< std::endl;

        std::stringstream * tmpis = new std::stringstream(inputs);
        fileStream.reset(tmpis);
	}

	~StringSource() {}

	XMLDocument *createReader(XMLDocument::Handler &handler) override
	{ return new XMLDocument(fileStream, handler); }

    private:
  std::auto_ptr<std::istream>	fileStream;
};

class LHEReader::XMLHandler : public XMLDocument::Handler {
    public:
  typedef std::vector<std::pair<std::string,std::string> > wgt_info;
	XMLHandler() :
		impl(0), gotObject(kNone), mode(kNone),
		xmlHeader(0), xmlEvent(0), headerOk(false), npLO(-99), npNLO(-99) {}
	~XMLHandler()
	{ if (xmlHeader) xmlHeader->release(); 
	  if (xmlEvent) xmlEvent->release();   }

	enum Object {
		kNone = 0,
		kHeader,
		kInit,
		kComment,
		kEvent
	};

        void reset() { headerOk = false; weightsinevent.clear();}

        const wgt_info& weightInfo() const {return weightsinevent;}

    protected:
	void startElement(const XMLCh *const uri,
	                  const XMLCh *const localname,
	                  const XMLCh *const qname,
	                  const Attributes &attributes) override;

	void endElement(const XMLCh *const uri,
	                const XMLCh *const localname,
	                const XMLCh *const qname) override;

	void characters(const XMLCh *const data, const unsigned int length) override;
	void comment(const XMLCh *const data, const unsigned int length) override;

    private:
	friend class LHEReader;

	DOMImplementation		*impl;
	std::string			buffer;
	Object				gotObject;
	Object				mode;
	DOMDocument			*xmlHeader;
        DOMDocument			*xmlEvent;
        std::vector<DOMElement*>	xmlNodes,xmlEventNodes;
	bool				headerOk;
	std::vector<LHERunInfo::Header>	headers;
        wgt_info weightsinevent;
        int                             npLO;
        int                             npNLO;
        std::map<int,float>             scalesmap;
};

static void attributesToDom(DOMElement *dom, const Attributes &attributes)
{
	for(unsigned int i = 0; i < attributes.getLength(); i++) {
		const XMLCh *name = attributes.getQName(i);
		const XMLCh *value = attributes.getValue(i);

		dom->setAttribute(name, value);
	}
}

static void fillHeader(LHERunInfo::Header &header, const char *data,
                       int len = -1)
{
	const char *end = len >= 0 ? (data + len) : 0;
	while(*data && (!end || data < end)) {
		std::size_t len = std::strcspn(data, "\r\n");
		if (end && data + len > end)
			len = end - data;
		if (data[len] == '\r' && data[len + 1] == '\n')
			len += 2;
		else if (data[len])
			len++;
		header.addLine(std::string(data, len));
		data += len;
	}
}

void LHEReader::XMLHandler::startElement(const XMLCh *const uri,
                                         const XMLCh *const localname,
                                         const XMLCh *const qname,
                                         const Attributes &attributes)
{
  std::string name((const char*)XMLSimpleStr(qname));

  if (!headerOk) {
    if (name != "LesHouchesEvents")
      throw cms::Exception("InvalidFormat")
	<< "LHE file has invalid header" << std::endl;
    headerOk = true;
    return;
  }
  
  if (mode == kHeader) {	  
    DOMElement *elem = xmlHeader->createElement(qname);
    attributesToDom(elem, attributes);
    xmlNodes.back()->appendChild(elem);
    xmlNodes.push_back(elem);
    return;
  } else if ( mode == kEvent ) {
    DOMElement *elem = xmlEvent->createElement(qname);    
    attributesToDom(elem, attributes);

    //TODO this is a hack (even more than the rest of this class)
    if( name == "rwgt" ) {
      xmlEventNodes[0]->appendChild(elem);
    } else if (name == "wgt") {
      xmlEventNodes[1]->appendChild(elem);
    }
    else if (name == "scales") {
      for (XMLSize_t iscale=0; iscale<attributes.getLength(); ++iscale) {
        int ipart = 0;
        const char *scalename = XMLSimpleStr(attributes.getQName(iscale));
        sscanf(scalename,"pt_clust_%d",&ipart);
        
        float scaleval;
        const char *scalevalstr = XMLSimpleStr(attributes.getValue(iscale));
        sscanf(scalevalstr,"%e",&scaleval);
        
        scalesmap[ipart] = scaleval;
      }
    }
    xmlEventNodes.push_back(elem);
    return;
  } else if (mode != kNone) {
    throw cms::Exception("InvalidFormat")
      << "LHE file has invalid format" << std::endl;
  }
  
  if (name == "header") {
    if (!impl)
      impl = DOMImplementationRegistry::getDOMImplementation(
							  XMLUniStr("Core"));
    xmlHeader = impl->createDocument(0, qname, 0);
    xmlNodes.resize(1);
    xmlNodes[0] = xmlHeader->getDocumentElement();
    mode = kHeader;
  } if (name == "init") {
      mode = kInit;
  } else if (name == "event") {
    if (!impl)
      impl = DOMImplementationRegistry::getDOMImplementation(
							  XMLUniStr("Core"));
    if(xmlEvent)  xmlEvent->release();
    xmlEvent = impl->createDocument(0, qname, 0);
    weightsinevent.resize(0);
    scalesmap.clear();
    
    npLO = -99;
    npNLO = -99;
    const XMLCh *npLOval = attributes.getValue(XMLString::transcode("npLO"));
    if (npLOval) {
      const char *npLOs = XMLSimpleStr(npLOval);      
      sscanf(npLOs,"%d",&npLO);
    }
    const XMLCh *npNLOval = attributes.getValue(XMLString::transcode("npNLO"));
    if (npNLOval) {
      const char *npNLOs = XMLSimpleStr(npNLOval);      
      sscanf(npNLOs,"%d",&npNLO);
    }    
    
    xmlEventNodes.resize(1);
    xmlEventNodes[0] = xmlEvent->getDocumentElement();
    mode = kEvent;    
  }
  
  if (mode == kNone)
    throw cms::Exception("InvalidFormat")
      << "LHE file has invalid format" << std::endl;
  
  buffer.clear();
}

void LHEReader::XMLHandler::endElement(const XMLCh *const uri,
                                       const XMLCh *const localname,
                                       const XMLCh *const qname)
{
  std::string name((const char*)XMLSimpleStr(qname));
  
  if (mode) {

    if (mode == kHeader && xmlNodes.size() > 1) {
      xmlNodes.resize(xmlNodes.size() - 1);
      return;
    } else if (mode == kHeader) {
      std::auto_ptr<DOMWriter> writer(
				      static_cast<DOMImplementationLS*>(
                                                impl)->createDOMWriter());
      writer->setEncoding(XMLUniStr("UTF-8"));
      
      for(DOMNode *node = xmlNodes[0]->getFirstChild();
	  node; node = node->getNextSibling()) {
	XMLSimpleStr buffer(writer->writeToString(*node));

	std::string type;
	const char *p, *q;
	DOMElement *elem;
	
	switch(node->getNodeType()) {
	case DOMNode::ELEMENT_NODE:	  
	    elem = static_cast<DOMElement*>(node);
	    type = (const char*)XMLSimpleStr(
					     elem->getTagName());
	    p = std::strchr((const char*)buffer,
			    '>') + 1;
	    q = std::strrchr(p, '<');	 
	  break;
	case DOMNode::COMMENT_NODE:
	  type = "";
	  p = buffer + 4;
	  q = buffer + strlen(buffer) - 3;
	  break;
	default:
	  type = "<>";
	  p = buffer +
	    std::strspn(buffer, " \t\r\n");
	  if (!*p)
	    continue;
	  q = p + strlen(p);
	}	
	LHERunInfo::Header header(type);
	fillHeader(header, p, q - p);
	headers.push_back(header);	
      }
      
      xmlHeader->release();
      xmlHeader = 0;
    }
    else if (name == "event" && 
	mode == kEvent && 
	xmlEventNodes.size() >= 1) { // handling of weights in LHE file
      for(DOMNode *node = xmlEventNodes[0]->getFirstChild();
	  node; node = node->getNextSibling()) {
	switch( node->getNodeType() ) {
	case DOMNode::ELEMENT_NODE: // rwgt
	  for(DOMNode *rwgt = xmlEventNodes[1]->getFirstChild();
	      rwgt; rwgt = rwgt->getNextSibling()) {
	    DOMNode* attr = rwgt->getAttributes()->item(0);
	    XMLSimpleStr atname(attr->getNodeValue());
	    XMLSimpleStr weight(rwgt->getFirstChild()->getNodeValue());
	    switch( rwgt->getNodeType() ) {
	    case DOMNode::ELEMENT_NODE:
	      weightsinevent.push_back(std::make_pair((const char*)atname,
						      (const char*)weight));
	      break;	    
	    default:	    	  
	      break;
	    }	  
	  }
	  break;
	case DOMNode::TEXT_NODE: // event information
	  {
	    XMLSimpleStr data(node->getNodeValue());
	    buffer.append(data);
	  }
	  break;
	default:
	  break;
	}
      }      
    }
    else if (mode == kEvent) {
      //skip unknown tags
      return;
    }

    if (gotObject != kNone)
      throw cms::Exception("InvalidState")
	<< "Unexpected pileup in"
	" LHEReader::XMLHandler::endElement"
	<< std::endl;
    
    gotObject = mode;
    mode = kNone;
  }
}

void LHEReader::XMLHandler::characters(const XMLCh *const data_,
                                       const unsigned int length)
{
	if (mode == kHeader) {
		DOMText *text = xmlHeader->createTextNode(data_);
		xmlNodes.back()->appendChild(text);
		return;
	}	

	if (XMLSimpleStr::isAllSpaces(data_, length))
		return;

	unsigned int offset = 0;
	while(offset < length && XMLSimpleStr::isSpace(data_[offset]))
		offset++;	

	if( mode == kEvent ) {
	  DOMText *text = xmlEvent->createTextNode(data_+offset);
	  xmlEventNodes.back()->appendChild(text);
	  return;
	}	

	if (mode == kNone)
		throw cms::Exception("InvalidFormat")
			<< "LHE file has invalid format" << std::endl;

	XMLSimpleStr data(data_ + offset);
	buffer.append(data);
}

void LHEReader::XMLHandler::comment(const XMLCh *const data_,
                                    const unsigned int length)
{
	if (mode == kHeader) {
		DOMComment *comment = xmlHeader->createComment(data_);
		xmlNodes.back()->appendChild(comment);
		return;
	}

	XMLSimpleStr data(data_);

	LHERunInfo::Header header;
	fillHeader(header, data);
	headers.push_back(header);
}

LHEReader::LHEReader(const edm::ParameterSet &params) :
	fileURLs(params.getUntrackedParameter< std::vector<std::string> >("fileNames")),
    strName(""),
	firstEvent(params.getUntrackedParameter<unsigned int>("skipEvents", 0)),
	maxEvents(params.getUntrackedParameter<int>("limitEvents", -1)),
	curIndex(0), handler(new XMLHandler())
{
}

LHEReader::LHEReader(const std::vector<std::string> &fileNames,
                     unsigned int firstEvent) :
  fileURLs(fileNames), strName(""), firstEvent(firstEvent), maxEvents(-1),
	curIndex(0), handler(new XMLHandler())
{
}

  LHEReader::LHEReader(const std::string &inputs,
                     unsigned int firstEvent) :
    strName(inputs), firstEvent(firstEvent), maxEvents(-1),
  curIndex(0), handler(new XMLHandler())
{
}

LHEReader::~LHEReader()
{
}

  boost::shared_ptr<LHEEvent> LHEReader::next(bool* newFileOpened)
  {
    while(curDoc.get() || curIndex < fileURLs.size() || (fileURLs.size() == 0 && strName != "" ) ) {
      if (!curDoc.get()) {
        if ( fileURLs.size() > 0 ) {
          logFileAction("  Initiating request to open LHE file ", fileURLs[curIndex]);
          curSource.reset(new FileSource(fileURLs[curIndex]));
          logFileAction("  Successfully opened LHE file ", fileURLs[curIndex]);
          if ( newFileOpened != nullptr ) *newFileOpened = true;
          ++curIndex;
        } else if ( strName != "" ) {
          curSource.reset(new StringSource(strName));
        }
        handler->reset();
        curDoc.reset(curSource->createReader(*handler));
        curRunInfo.reset();
      }
    
      XMLHandler::Object event = handler->gotObject;
      handler->gotObject = XMLHandler::kNone;
    
      std::istringstream data;
      if (event != XMLHandler::kNone) {
        data.str(handler->buffer);
        handler->buffer.clear();
      }
        
      switch(event) {
      case XMLHandler::kNone:
        if (!curDoc->parse()) {
          curDoc.reset();
          logFileAction("  Closed LHE file ", fileURLs[curIndex - 1]);
          return boost::shared_ptr<LHEEvent>();
        }
        break;
        
      case XMLHandler::kHeader:
        break;
        
      case XMLHandler::kInit:
        curRunInfo.reset(new LHERunInfo(data));
		
        std::for_each(handler->headers.begin(),
                      handler->headers.end(),
                      boost::bind(&LHERunInfo::addHeader,
                                  curRunInfo.get(), _1));
        handler->headers.clear();
        break;
        
      case XMLHandler::kComment:
        break;
        
      case XMLHandler::kEvent:
        if (!curRunInfo.get())
          throw cms::Exception("InvalidState")
            << "Got LHE event without"
            " initialization." << std::endl;

        if (firstEvent > 0) {
          firstEvent--;
          continue;
        }
        
        if (maxEvents == 0)
          return boost::shared_ptr<LHEEvent>();
        else if (maxEvents > 0)
          maxEvents--;

	boost::shared_ptr<LHEEvent> lheevent;
	lheevent.reset(new LHEEvent(curRunInfo, data));
	const XMLHandler::wgt_info& info = handler->weightsinevent;
	for( size_t i=0; i< info.size(); ++i ) {
	  std::string snum = info[i].second.substr(0,info[i].second.size()-1);
	  double num = -1.0;
	  sscanf(snum.c_str(),"%le",&num);	  
	  lheevent->addWeight(gen::WeightsInfo(info[i].first,num));
	}
	lheevent->setNpLO(handler->npLO);
        lheevent->setNpNLO(handler->npNLO);
        //fill scales
        if (handler->scalesmap.size()>0) {
          std::vector<float> &scales = lheevent->scales();
          scales = std::vector<float>(lheevent->getHEPEUP()->NUP, -1.);
          for (const std::pair<int,float> &scale : handler->scalesmap) {
            //index differs by one
            scales[scale.first-1] = scale.second;
          }
        }
        return lheevent;
      }
    }
    
    return boost::shared_ptr<LHEEvent>();
  }
  
} // namespace lhef
  
