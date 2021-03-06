#include <filter.hh>
#include <configuration.hh>
#include <utils.hh>

#include <limits.h>
#include <stdlib.h>
#include <string>
#include <vector>

using namespace kcov;

class DummyFilter : public IFilter
{
public:
	DummyFilter()
	{
	}

	~DummyFilter()
	{
	}

	// Allow anything
	bool runFilters(const std::string &file)
	{
		return true;
	}

	std::string mangleSourcePath(const std::string &path)
	{
		return path;
	}

};

class Filter : public IFilter
{
public:
	Filter()
	{
		m_patternHandler = new PatternHandler();
		m_pathHandler = new PathHandler();

		m_origRoot = IConfiguration::getInstance().keyAsString("orig-path-prefix");
		m_newRoot  = IConfiguration::getInstance().keyAsString("new-path-prefix");
	}

	~Filter()
	{
		delete m_patternHandler;
		delete m_pathHandler;
	}

	// Used by the unit test
	void setup()
	{
		delete m_patternHandler;
		delete m_pathHandler;
		m_patternHandler = new PatternHandler();
		m_pathHandler = new PathHandler();
	}

	bool runFilters(const std::string &file)
	{
		bool out = true;

		if (m_pathHandler->isSetup())
			out = m_pathHandler->includeFile(file);

		if (out && m_patternHandler->isSetup())
			return m_patternHandler->includeFile(file);

		return out;
	}

	std::string mangleSourcePath(const std::string &path)
	{
		std::string filename = get_real_path(path);

		if (m_origRoot.length() > 0 && m_newRoot.length() > 0) {
		    std::string path = filename;
		    size_t index = path.find(m_origRoot);

		    if (index != std::string::npos) {
		        path.replace(index, m_origRoot.length(), m_newRoot);
		        filename = get_real_path(path);
		    }
		}

		return filename;
	}

private:
	class PatternHandler
	{
	public:
		PatternHandler() :
			m_includePatterns(IConfiguration::getInstance().keyAsList("include-pattern")),
			m_excludePatterns(IConfiguration::getInstance().keyAsList("exclude-pattern"))
		{
		}

		bool isSetup()
		{
			return !(m_includePatterns.size() == 0 && m_excludePatterns.size() == 0);
		}

		bool includeFile(std::string file)
		{
			if (m_includePatterns.size() == 0 && m_excludePatterns.size() == 0)
				return true;

			bool out = true;

			if (m_includePatterns.size() != 0)
				out = false;

			for (PatternMap_t::const_iterator it = m_includePatterns.begin();
					it != m_includePatterns.end();
					++it) {
				const std::string &pattern = *it;

				if (file.find(pattern) != std::string::npos)
					out = true;
			}

			for (PatternMap_t::const_iterator it = m_excludePatterns.begin();
					it != m_excludePatterns.end();
					++it) {
				const std::string &pattern = *it;

				if (file.find(pattern) != std::string::npos)
					out = false;
			}

			return out;
		}
	private:
		typedef std::vector<std::string> PatternMap_t;

		const PatternMap_t &m_includePatterns;
		const PatternMap_t &m_excludePatterns;
	};


	class PathHandler
	{
	public:
		PathHandler() :
			m_includePaths(IConfiguration::getInstance().keyAsList("include-path")),
			m_excludePaths(IConfiguration::getInstance().keyAsList("exclude-path"))
		{
			for (PathMap_t::iterator it = m_includePaths.begin();
					it != m_includePaths.end();
					++it)
				*it = get_real_path(*it);

			for (PathMap_t::iterator it = m_excludePaths.begin();
					it != m_excludePaths.end();
					++it)
				*it = get_real_path(*it);
		}

		bool isSetup()
		{
			return !(m_includePaths.size() == 0 && m_excludePaths.size() == 0);
		}

		bool includeFile(const std::string &file)
		{
			if (m_includePaths.size() == 0 && m_excludePaths.size() == 0)
				return true;

			bool out = true;

			if (m_includePaths.size() != 0)
				out = false;

			const std::string pathStr = get_real_path(file);

			// If found in --include-path=, accept this path
			for (PathMap_t::const_iterator it = m_includePaths.begin();
					it != m_includePaths.end();
					++it) {
				const std::string &pathPattern = *it;
				size_t last = pathPattern.size();

				if (pathStr.find(pathPattern) == 0 &&
						(pathStr.size() <= last || pathStr[last] == '/'))
					out = true;
			}

			// ... unless it's found in --exclude-path=.
			for (PathMap_t::const_iterator it = m_excludePaths.begin();
					it != m_excludePaths.end();
					++it) {
				const std::string &pathPattern = *it;
				size_t last = pathPattern.size();

				if (pathStr.find(pathPattern) == 0 &&
						(pathStr.size() <= last || pathStr[last] == '/'))
					out = false;
			}

			return out;
		}
	private:
		typedef std::vector<std::string> PathMap_t;

		PathMap_t m_includePaths;
		PathMap_t m_excludePaths;
	};


	PatternHandler *m_patternHandler;
	PathHandler *m_pathHandler;
	std::string m_origRoot;
	std::string m_newRoot;
};


IFilter &IFilter::create()
{
	return *new Filter();
}

IFilter &IFilter::createDummy()
{
	return *new DummyFilter();
}
