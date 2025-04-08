# Copyright (C) 2019 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import flask
import logging
import os
import re
import requests
import time
import json

from collections import namedtuple
from config import GITHUB_REPO, PROJECT
from common_utils import SCOPES, get_github_installation_token, req_async, utc_now_iso, get_github_registration_token
from stackdriver_metrics import STACKDRIVER_METRICS
''' Makes anonymous GET-only requests to Gerrit.

Solves the lack of CORS headers from AOSP gerrit.
'''

STACKDRIVER_API = 'https://monitoring.googleapis.com/v3/projects/%s' % PROJECT

SCOPES.append('https://www.googleapis.com/auth/cloud-platform')
# SCOPES.append('https://www.googleapis.com/auth/userinfo.email')
SCOPES.append('https://www.googleapis.com/auth/datastore')
SCOPES.append('https://www.googleapis.com/auth/monitoring')
SCOPES.append('https://www.googleapis.com/auth/monitoring.write')

HASH_RE = re.compile('^[a-f0-9]+$')
CACHE_TTL = 3600  # 1 h
CacheEntry = namedtuple('CacheEntry', ['contents', 'expiration'])

app = flask.Flask(__name__)

logging.basicConfig(
    format='%(levelname)-8s %(asctime)s %(message)s',
    level=logging.DEBUG if os.getenv('VERBOSE') else logging.INFO,
    datefmt=r'%Y-%m-%d %H:%M:%S')

cache = {}


def DeleteStaleCacheEntries():
  now = time.time()
  for url, entry in list(cache.items()):
    if now > entry.expiration:
      cache.pop(url, None)


@app.route('/_ah/start', methods=['GET', 'POST'])
async def http_start():
  await create_stackdriver_metric_definitions()
  return 'OK'


async def create_stackdriver_metric_definitions():
  logging.info('Creating Stackdriver metric definitions')
  for name, metric in STACKDRIVER_METRICS.items():
    logging.info('Creating metric %s', name)
    await req_async('POST', STACKDRIVER_API + '/metricDescriptors', body=metric)


@app.route('/gh/runners', methods=['GET', 'POST'])
async def gh_runners():
  url = f'https://api.github.com/repos/{GITHUB_REPO}/actions/runners'
  params = {'per_page': 100}
  headers = {
      'Authorization': f'token {get_github_installation_token()}',
      'Accept': 'application/vnd.github+json'
  }
  resp = requests.get(url, headers=headers, params=params)
  return resp.content.decode('utf-8')


@app.route('/gh/jobs', methods=['GET', 'POST'])
async def gh_jobs():
  url = f'https://api.github.com/repos/{GITHUB_REPO}/actions/runs'
  params = {'per_page': 100}
  headers = {
      'Authorization': f'token {get_github_installation_token()}',
      'Accept': 'application/vnd.github+json'
  }
  resp = requests.get(url, headers=headers, params=params)
  return resp.content.decode('utf-8')


@app.route('/gh/purge_runners', methods=['GET', 'POST'])
async def gh_purge_runners():
  headers = {
      'Authorization': f'token {get_github_installation_token()}',
      'Accept': 'application/vnd.github+json'
  }
  js = json.loads(await gh_runners())
  deleted = 0
  for r in js.get('runners', []):
    if r['status'] != 'offline':
      continue
    id = str(r['id'])
    url = f'https://api.github.com/repos/{GITHUB_REPO}/actions/runners/${id}'
    import sys
    resp = requests.delete(url, headers=headers)
    deleted += 1 if resp.status_code == 204 else 0
  return str(deleted)


@app.route('/gh/pulls', methods=['GET', 'POST'])
async def gh_pulls():
  url = f'https://api.github.com/repos/{GITHUB_REPO}/pulls'
  params = {
      'state': 'all',
      'per_page': 50,
      'sort': 'updated',
      'direction': 'desc'
  }
  headers = {
      'Authorization': f'token {get_github_installation_token()}',
      'Accept': 'application/vnd.github+json'
  }
  resp = requests.get(url, headers=headers, params=params)
  return resp.content.decode('utf-8')


@app.route('/gh/checks/<string:sha>', methods=['GET', 'POST'])
async def gh_checks(sha):
  url = f'https://api.github.com/repos/{GITHUB_REPO}/commits/{sha}/check-runs'
  params = {}
  headers = {
      'Authorization': f'token {get_github_installation_token()}',
      'Accept': 'application/vnd.github+json'
  }
  resp = requests.get(url, headers=headers, params=params)
  return resp.content.decode('utf-8')


@app.route('/gh/patchsets/<int:pr>', methods=['GET', 'POST'])
async def gh_patchsets(pr):
  url = f'https://api.github.com/repos/{GITHUB_REPO}/pulls/{pr}/commits'
  params = {}
  headers = {
      'Authorization': f'token {get_github_installation_token()}',
      'Accept': 'application/vnd.github+json'
  }
  resp = requests.get(url, headers=headers, params=params)
  return resp.content.decode('utf-8')


@app.route('/gh/commits/main', methods=['GET', 'POST'])
async def gh_commits_main():
  url = f'https://api.github.com/repos/{GITHUB_REPO}/commits'
  params = {'sha': 'main'}
  headers = {
      'Authorization': f'token {get_github_installation_token()}',
      'Accept': 'application/vnd.github+json'
  }
  resp = requests.get(url, headers=headers, params=params)
  return resp.content.decode('utf-8')


@app.route('/gh/update_metrics', methods=['GET', 'POST'])
async def update_metrics():
  url = f'https://api.github.com/repos/{GITHUB_REPO}/actions/runs'
  params = {'per_page': 100}
  headers = {
      'Authorization': f'token {get_github_installation_token()}',
      'Accept': 'application/vnd.github+json'
  }
  resp = requests.get(url, headers=headers, params=params)
  txt = resp.content.decode('utf-8')
  respj = json.loads(txt)
  num_pending = 0
  for w in respj.get('workflow_runs', []):
    if w['status'] in ('queued', 'in_progress'):
      num_pending += 1
  await write_metrics({'ci_job_queue_len': {'v': num_pending}})
  return str(num_pending)


async def write_metrics(metric_dict):
  now = utc_now_iso()
  desc = {'timeSeries': []}
  for key, spec in metric_dict.items():
    desc['timeSeries'] += [{
        'metric': {
            'type': STACKDRIVER_METRICS[key]['type'],
            'labels': spec.get('l', {})
        },
        'resource': {
            'type': 'global'
        },
        'points': [{
            'interval': {
                'endTime': now
            },
            'value': {
                'int64Value': str(spec['v'])
            }
        }]
    }]
  try:
    await req_async('POST', STACKDRIVER_API + '/timeSeries', body=desc)
  except Exception as e:
    # Metric updates can easily fail due to Stackdriver API limitations.
    msg = str(e)
    if 'written more frequently than the maximum sampling' not in msg:
      logging.error('Metrics update failed: %s', msg)
