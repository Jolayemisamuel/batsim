/**
 * @file jobs.cpp
 * @brief Contains job-related structures
 */

#include "jobs.hpp"
#include "workload.hpp"

#include <string>
#include <fstream>
#include <streambuf>
#include <algorithm>
#include <boost/regex.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/join.hpp>

#include <simgrid/msg.h>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include "profiles.hpp"

using namespace std;
using namespace rapidjson;

XBT_LOG_NEW_DEFAULT_CATEGORY(jobs, "jobs"); //!< Logging


BatTask::BatTask(Job * parent_job, Profile * profile) :
    parent_job(parent_job),
    profile(profile)
{
}

BatTask::~BatTask()
{
    for (auto & sub_btask : this->sub_tasks)
    {
        delete sub_btask;
        sub_btask = nullptr;
    }
}

void BatTask::compute_leaf_progress()
{
    xbt_assert(sub_tasks.empty(), "Leaves should not contain sub tasks");

    if (profile->is_parallel_task())
    {
        xbt_assert(ptask != nullptr, "Internal error");

        // WARNING: This is not returning the flops amount but the remainin quantity of work
        // from 1 (not started yet) to 0 (completely finished)
        current_task_progress_ratio = 1 - MSG_task_get_flops_amount(ptask);
    }
    else if (profile->type == ProfileType::DELAY)
    {
        xbt_assert(delay_task_start != -1, "Internal error");

        double runtime = MSG_get_clock() - delay_task_start;

        // Manages empty delay job (why?!)
        if (delay_task_required == 0)
        {
            current_task_progress_ratio = 1;
        }
        else
        {
            current_task_progress_ratio = runtime / delay_task_required;
        }
    }
    else
    {
        XBT_WARN("Computing the progress of %s profiles is not implemented.",
                 profile_type_to_string(profile->type).c_str());
    }
}

void BatTask::compute_tasks_progress()
{
    if (profile->type == ProfileType::SEQUENCE)
    {
        sub_tasks[current_task_index]->compute_tasks_progress();
    }
    else
    {
        this->compute_leaf_progress();
    }
}

BatTask* Job::compute_job_progress()
{
    xbt_assert(task != nullptr, "Internal error");

    task->compute_tasks_progress();
    return task;
}


Jobs::Jobs()
{

}

Jobs::~Jobs()
{
    for (auto mit : _jobs)
    {
        delete mit.second;
    }
}

void Jobs::set_profiles(Profiles *profiles)
{
    _profiles = profiles;
}

void Jobs::set_workload(Workload *workload)
{
    _workload = workload;
}

void Jobs::load_from_json(const Document &doc, const string &filename)
{
    string error_prefix = "Invalid JSON file '" + filename + "'";

    xbt_assert(doc.IsObject(), "%s: not a JSON object", error_prefix.c_str());
    xbt_assert(doc.HasMember("jobs"), "%s: the 'jobs' array is missing", error_prefix.c_str());
    const Value & jobs = doc["jobs"];
    xbt_assert(jobs.IsArray(), "%s: the 'jobs' member is not an array", error_prefix.c_str());

    for (SizeType i = 0; i < jobs.Size(); i++) // Uses SizeType instead of size_t
    {
        const Value & job_json_description = jobs[i];

        Job * j = Job::from_json(job_json_description, _workload, error_prefix);

        xbt_assert(!exists(j->number), "%s: duplication of job id %d",
                   error_prefix.c_str(), j->number);
        _jobs[j->number] = j;
    }
}

Job *Jobs::operator[](int job_number)
{
    auto it = _jobs.find(job_number);
    xbt_assert(it != _jobs.end(), "Cannot get job %d: it does not exist", job_number);
    return it->second;
}

const Job *Jobs::operator[](int job_number) const
{
    auto it = _jobs.find(job_number);
    xbt_assert(it != _jobs.end(), "Cannot get job %d: it does not exist", job_number);
    return it->second;
}

Job *Jobs::at(int job_number)
{
    return operator[](job_number);
}

const Job *Jobs::at(int job_number) const
{
    return operator[](job_number);
}

void Jobs::add_job(Job *job)
{
    xbt_assert(!exists(job->number),
               "Bad Jobs::add_job call: A job with number=%d already exists.",
               job->number);

    _jobs[job->number] = job;
}

bool Jobs::exists(int job_number) const
{
    auto it = _jobs.find(job_number);
    return it != _jobs.end();
}

bool Jobs::contains_smpi_job() const
{
    xbt_assert(_profiles != nullptr, "Invalid Jobs::containsSMPIJob call: setProfiles had not been called yet");
    for (auto & mit : _jobs)
    {
        Job * job = mit.second;
        if ((*_profiles)[job->profile]->type == ProfileType::SMPI)
        {
            return true;
        }
    }
    return false;
}

void Jobs::displayDebug() const
{
    // Let us traverse jobs to display some information about them
    vector<string> jobsVector;
    for (auto & mit : _jobs)
    {
        jobsVector.push_back(std::to_string(mit.second->number));
    }

    // Let us create the string that will be sent to XBT_INFO
    string s = "Jobs debug information:\n";

    s += "There are " + to_string(_jobs.size()) + " jobs.\n";
    s += "Jobs : [" + boost::algorithm::join(jobsVector, ", ") + "]";

    // Let us display the string which has been built
    XBT_INFO("%s", s.c_str());
}

const std::map<int, Job* > &Jobs::jobs() const
{
    return _jobs;
}

std::map<int, Job *> &Jobs::jobs()
{
    return _jobs;
}

int Jobs::nb_jobs() const
{
    return _jobs.size();
}

bool job_comparator_subtime_number(const Job *a, const Job *b)
{
    if (a->submission_time == b->submission_time)
    {
        return a->number < b->number;
    }
    return a->submission_time < b->submission_time;
}

Job::~Job()
{
    xbt_assert(execution_processes.size() == 0,
               "Internal error: job %s has %d execution processes on destruction (should be 0).",
               this->id.to_string().c_str(), (int)execution_processes.size());

    if (task != nullptr)
    {
        delete task;
        task = nullptr;
    }
}

bool Job::is_complete() const
{
    return (state == JobState::JOB_STATE_COMPLETED_SUCCESSFULLY) ||
           (state == JobState::JOB_STATE_COMPLETED_KILLED) ||
           (state == JobState::JOB_STATE_COMPLETED_FAILED) ||
           (state == JobState::JOB_STATE_COMPLETED_WALLTIME_REACHED);
}

// Do NOT remove namespaces in the arguments (to avoid doxygen warnings)
Job * Job::from_json(const rapidjson::Value & json_desc,
                     Workload * workload,
                     const std::string & error_prefix)
{
    Job * j = new Job;
    j->workload = workload;
    j->starting_time = -1;
    j->runtime = -1;
    j->state = JobState::JOB_STATE_NOT_SUBMITTED;
    j->consumed_energy = -1;
    string workload_name;

    xbt_assert(json_desc.IsObject(), "%s: one job is not an object", error_prefix.c_str());

    xbt_assert(json_desc.HasMember("id"), "%s: one job has no 'id' field", error_prefix.c_str());
    if (json_desc["id"].IsInt())
    {
        j->number = json_desc["id"].GetInt();
        workload_name = workload->name;
    }
    else if (json_desc["id"].IsString())
    {
        const string job_id_str = json_desc["id"].GetString();

        vector<string> job_identifier_parts;
        boost::split(job_identifier_parts, job_id_str, boost::is_any_of("!"), boost::token_compress_on);
        xbt_assert(job_identifier_parts.size() == 2,
                   "%s: Invalid string job identifier '%s': should be formatted as two '!'-separated "
                   "parts, the second one being an integral number. Example: 'some_text!42'.",
                   error_prefix.c_str(), job_id_str.c_str());

        workload_name = job_identifier_parts[0];
        XBT_DEBUG("========  %s : %s", workload->name.c_str(), workload_name.c_str());
        xbt_assert(workload_name == workload->name);
        j->number = std::stoi(job_identifier_parts[1]);
    }
    else
    {
        XBT_ERROR("%s: job %d id is neither a string nor an integer",
                  error_prefix.c_str(), j->number);
        xbt_abort();
    }

    xbt_assert(json_desc.HasMember("subtime"), "%s: job %d has no 'subtime' field",
               error_prefix.c_str(), j->number);
    xbt_assert(json_desc["subtime"].IsNumber(), "%s: job %d has a non-number 'subtime' field",
               error_prefix.c_str(), j->number);
    j->submission_time = json_desc["subtime"].GetDouble();


    // Make walltime optional
    if (!json_desc.HasMember("walltime"))
    {
        XBT_INFO("job %d has no 'walltime' field", j->number);
    }
    else
    {
        xbt_assert(json_desc["walltime"].IsNumber(), "%s: job %d has a non-number 'walltime' field",
                   error_prefix.c_str(), j->number);
        j->walltime = json_desc["walltime"].GetDouble();
    }
    xbt_assert(j->walltime == -1 || j->walltime > 0,
               "%s: job %d has an invalid walltime (%g). It should either be -1 (no walltime) "
               "or a strictly positive number.",
               error_prefix.c_str(), j->number, (double)j->walltime);

    xbt_assert(json_desc.HasMember("res"), "%s: job %d has no 'res' field",
               error_prefix.c_str(), j->number);
    xbt_assert(json_desc["res"].IsInt(), "%s: job %d has a non-number 'res' field",
               error_prefix.c_str(), j->number);
    j->required_nb_res = json_desc["res"].GetInt();

    xbt_assert(json_desc.HasMember("profile"), "%s: job %d has no 'profile' field",
               error_prefix.c_str(), j->number);
    xbt_assert(json_desc["profile"].IsString(), "%s: job %d has a non-string 'profile' field",
               error_prefix.c_str(), j->number);
    j->profile = json_desc["profile"].GetString();

    // Let's get the JSON string which originally described the job
    // (to conserve potential fields unused by Batsim)
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    json_desc.Accept(writer);

    // Let's replace the job ID by its WLOAD!NUMBER counterpart if needed
    string json_description_tmp(buffer.GetString(), buffer.GetSize());
    boost::regex r(R"foo("id"\s*:\s*(?:\d+)\s*)foo");
    string replacement_str = "\"id\":\"" + workload_name + "!" + std::to_string(j->number) + "\"";
    j->json_description = boost::regex_replace(json_description_tmp, r, replacement_str);

    // Let's check that the new description is a valid JSON string
    rapidjson::Document check_doc;
    check_doc.Parse(j->json_description.c_str());
    xbt_assert(!check_doc.HasParseError(),
               "A problem occured when replacing the job_id by its WLOAD!job_number counterpart:"
               "The output string '%s' is not valid JSON.", j->json_description.c_str());
    xbt_assert(check_doc.IsObject(),
               "A problem occured when replacing the job_id by its WLOAD!job_number counterpart: "
               "The output string '%s' is not valid JSON.", j->json_description.c_str());
    xbt_assert(check_doc.HasMember("id"),
               "A problem occured when replacing the job_id by its WLOAD!job_number counterpart: "
               "The output JSON '%s' has no 'id' field.", j->json_description.c_str());
    xbt_assert(check_doc["id"].IsString(),
               "A problem occured when replacing the job_id by its WLOAD!job_number counterpart: "
               "The output JSON '%s' has a non-string 'id' field.", j->json_description.c_str());
    xbt_assert(check_doc.HasMember("subtime") && check_doc["subtime"].IsNumber(),
               "A problem occured when replacing the job_id by its WLOAD!job_number counterpart: "
               "The output JSON '%s' has no 'subtime' field (or it is not a number)",
               j->json_description.c_str());
    xbt_assert((check_doc.HasMember("walltime") && check_doc["walltime"].IsNumber())
               || (!check_doc.HasMember("walltime")),
               "A problem occured when replacing the job_id by its WLOAD!job_number counterpart: "
               "The output JSON '%s' has no 'walltime' field (or it is not a number)",
               j->json_description.c_str());
    xbt_assert(check_doc.HasMember("res") && check_doc["res"].IsInt(),
               "A problem occured when replacing the job_id by its WLOAD!job_number counterpart: "
               "The output JSON '%s' has no 'res' field (or it is not an integer)",
               j->json_description.c_str());
    xbt_assert(check_doc.HasMember("profile") && check_doc["profile"].IsString(),
               "A problem occured when replacing the job_id by its WLOAD!job_number counterpart: "
               "The output JSON '%s' has no 'profile' field (or it is not a string)",
               j->json_description.c_str());

    if (json_desc.HasMember("smpi_ranks_to_hosts_mapping"))
    {
        xbt_assert(json_desc["smpi_ranks_to_hosts_mapping"].IsArray(),
                "%s: job %d has a non-array 'smpi_ranks_to_hosts_mapping' field",
                error_prefix.c_str(), j->number);

        const auto & mapping_array = json_desc["smpi_ranks_to_hosts_mapping"];
        j->smpi_ranks_to_hosts_mapping.resize(mapping_array.Size());

        for (unsigned int i = 0; i < mapping_array.Size(); ++i)
        {
            xbt_assert(mapping_array[i].IsInt(),
                       "%s: job %d has a bad 'smpi_ranks_to_hosts_mapping' field: rank "
                       "%d does not point to an integral number",
                       error_prefix.c_str(), j->number, i);
            int host_number = mapping_array[i].GetInt();
            xbt_assert(host_number >= 0 && host_number < j->required_nb_res,
                       "%s: job %d has a bad 'smpi_ranks_to_hosts_mapping' field: rank "
                       "%d has an invalid value %d : should be in [0,%d[",
                       error_prefix.c_str(), j->number, i, host_number, j->required_nb_res);

            j->smpi_ranks_to_hosts_mapping[i] = host_number;
        }
    }

    XBT_DEBUG("Loaded job %d from workload %s", (int) j->number, j->workload->name.c_str());
    return j;
}

// Do NOT remove namespaces in the arguments (to avoid doxygen warnings)
Job * Job::from_json(const std::string & json_str,
                     Workload * workload,
                     const std::string & error_prefix)
{
    Document doc;
    doc.Parse(json_str.c_str());
    xbt_assert(!doc.HasParseError(),
               "%s: Cannot be parsed. Content (between '##'):\n#%s#",
               error_prefix.c_str(), json_str.c_str());

    return Job::from_json(doc, workload, error_prefix);
}

string job_state_to_string(const JobState & state)
{
    string job_state("UNKNOWN");

    switch (state)
    {
    case JobState::JOB_STATE_NOT_SUBMITTED:
        job_state = "NOT_SUBMITTED";
        break;
    case JobState::JOB_STATE_SUBMITTED:
        job_state = "SUBMITTED";
        break;
    case JobState::JOB_STATE_RUNNING:
        job_state = "RUNNING";
        break;
    case JobState::JOB_STATE_COMPLETED_SUCCESSFULLY:
        job_state = "COMPLETED_SUCCESSFULLY";
        break;
    case JobState::JOB_STATE_COMPLETED_FAILED:
        job_state = "COMPLETED_FAILED";
        break;
    case JobState::JOB_STATE_COMPLETED_WALLTIME_REACHED:
        job_state = "COMPLETED_WALLTIME_REACHED";
        break;
    case JobState::JOB_STATE_COMPLETED_KILLED:
        job_state = "COMPLETED_KILLED";
        break;
    case JobState::JOB_STATE_REJECTED:
        job_state = "REJECTED";
        break;
    }
    return job_state;
}

JobState job_state_from_string(const std::string & state)
{
    JobState new_state;

    if (state == "NOT_SUBMITTED")
    {
        new_state = JobState::JOB_STATE_NOT_SUBMITTED;
    }
    else if (state == "SUBMITTED")
    {
        new_state = JobState::JOB_STATE_SUBMITTED;
    }
    else if (state == "RUNNING")
    {
        new_state = JobState::JOB_STATE_RUNNING;
    }
    else if (state == "COMPLETED_SUCCESSFULLY")
    {
        new_state = JobState::JOB_STATE_COMPLETED_SUCCESSFULLY;
    }
    else if (state == "COMPLETED_FAILED")
    {
        new_state = JobState::JOB_STATE_COMPLETED_FAILED;
    }
    else if (state == "COMPLETED_KILLED")
    {
        new_state = JobState::JOB_STATE_COMPLETED_KILLED;
    }
    else if (state == "COMPLETED_WALLTIME_REACHED")
    {
        new_state = JobState::JOB_STATE_COMPLETED_WALLTIME_REACHED;
    }
    else if (state == "REJECTED")
    {
        new_state = JobState::JOB_STATE_REJECTED;
    }
    else
    {
        xbt_assert(false, "Invalid job state");
    }

    return new_state;
}
